
/*
 * Copyright (C) sunlei
 */

#include "quic_ngx_http_server.h"

#include <errno.h>
#include <features.h>
#include <string.h>
#include <cstdint>
#include <memory>

#include "net/third_party/quiche/src/quic/core/crypto/crypto_handshake.h"
#include "net/third_party/quiche/src/quic/core/crypto/quic_random.h"
#include "net/third_party/quiche/src/quic/core/quic_crypto_stream.h"
#include "net/third_party/quiche/src/quic/core/quic_data_reader.h"
#include "net/third_party/quiche/src/quic/core/quic_packets.h"
#include "net/third_party/quiche/src/quic/core/tls_server_handshaker.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_flags.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_logging.h"
#include "net/third_party/quiche/src/quic/tools/quic_simple_crypto_server_stream_helper.h"
#include "quic_ngx_alarm_factory.h"
#include "quic_ngx_http_stream.h"
#include "quic_ngx_http_dispatcher.h"
#include "quic_ngx_http_backend.h"
#include "quic_ngx_packet_reader.h"
#include "quic_ngx_packet_writer.h"


namespace quic {


namespace {
  
const char kSourceAddressTokenSecret[] = "secret";

}  // namespace
  

const size_t kNumSessionsToCreatePerSocketEvent = 16;

QuicNgxHttpServer::QuicNgxHttpServer(std::unique_ptr<ProofSource> proof_source,
                             QuicNgxHttpBackend* quic_ngx_server_backend,
                             int idle_network_timeout)
  : QuicNgxHttpServer(std::move(proof_source),
                  QuicConfig(),
                  QuicCryptoServerConfig::ConfigOptions(),
                  AllSupportedVersions(),
                  quic_ngx_server_backend,
                  idle_network_timeout,
                  kQuicDefaultConnectionIdLength) {}

QuicNgxHttpServer::QuicNgxHttpServer(std::unique_ptr<ProofSource> proof_source,
                             const QuicConfig& config,
                             const ParsedQuicVersionVector& supported_versions,
                             QuicNgxHttpBackend* quic_ngx_server_backend,
                             int idle_network_timeout)
  : QuicNgxHttpServer(std::move(proof_source),
                  config,
                  QuicCryptoServerConfig::ConfigOptions(),
                  supported_versions,
                  quic_ngx_server_backend,
                  idle_network_timeout,
                  kQuicDefaultConnectionIdLength) {}

QuicNgxHttpServer::QuicNgxHttpServer(
    std::unique_ptr<ProofSource> proof_source,
    const QuicConfig& config,
    const QuicCryptoServerConfig::ConfigOptions& crypto_config_options,
    const ParsedQuicVersionVector& supported_versions,
    QuicNgxHttpBackend* quic_ngx_server_backend,
    int idle_network_timeout,
    uint8_t expected_connection_id_length)
  : port_(0),
    fd_(-1),
    packets_dropped_(0),
    overflow_supported_(false),
    silent_close_(false),
    config_(config),
    crypto_config_(kSourceAddressTokenSecret,
                   QuicRandom::GetInstance(),
                   std::move(proof_source),
                   KeyExchangeSource::Default()),
    crypto_config_options_(crypto_config_options),
    version_manager_(supported_versions),
    packet_reader_(new QuicNgxPacketReader()),
    quic_ngx_server_backend_(quic_ngx_server_backend),
    expected_connection_id_length_(expected_connection_id_length),
    helper_(new net::QuicChromiumConnectionHelper(&clock_,
            quic::QuicRandom::GetInstance())),
    writer_(nullptr),
    ngx_module_context_(nullptr),
    set_epoll_out_(nullptr) {
  if (-1 != idle_network_timeout) {
    config_.SetIdleNetworkTimeout(QuicTime::Delta::FromSeconds(idle_network_timeout));
  }
}

QuicNgxHttpServer::~QuicNgxHttpServer() = default;

void QuicNgxHttpServer::Initialize(void* ngx_module_context,
                               int listen_fd,
                               int port,
                               int address_family,
                               CreateNgxTimer create_ngx_timer,
                               AddNgxTimer add_ngx_timer,
                               DelNgxTimer del_ngx_timer,
                               FreeNgxTimer free_ngx_timer,
                               SetEPOLLOUT set_epoll_out) {
  // If an initial flow control window has not explicitly been set, then use a
  // sensible value for a server: 1 MB for session, 64 KB for each stream.
  const uint32_t kInitialSessionFlowControlWindow = 1 * 1024 * 1024;  // 1 MB
  const uint32_t kInitialStreamFlowControlWindow = 64 * 1024;         // 64 KB
  if (config_.GetInitialStreamFlowControlWindowToSend() ==
      kMinimumFlowControlSendWindow) {
    config_.SetInitialStreamFlowControlWindowToSend(
        kInitialStreamFlowControlWindow);
  }
  if (config_.GetInitialSessionFlowControlWindowToSend() ==
      kMinimumFlowControlSendWindow) {
    config_.SetInitialSessionFlowControlWindowToSend(
        kInitialSessionFlowControlWindow);
  }

  ngx_module_context_ = ngx_module_context;
  set_epoll_out_ = set_epoll_out;
  fd_ = listen_fd;
  port_ = port;


  int get_overflow = 1;
  int rc = setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &get_overflow,
                      sizeof(get_overflow));
  if (rc < 0) {
    QUIC_DLOG(WARNING) << "Socket overflow detection not supported";
  } else {
    overflow_supported_ = true;
  }

  rc = QuicSocketUtils::SetGetAddressInfo(fd_, address_family);
  if (rc < 0) {
    LOG(ERROR) << "IP detection not supported" << strerror(errno);
    exit(0);
  }

  rc = QuicSocketUtils::SetGetSoftwareReceiveTimestamp(fd_);
  if (rc < 0) {
    QUIC_LOG(WARNING) << "SO_TIMESTAMPING not supported; using fallback: "
                      << strerror(errno);
  }

  
  do {
    std::unique_ptr<CryptoHandshakeMessage> scfg(
      crypto_config_.AddDefaultConfig(helper_->GetRandomGenerator(),
                                      helper_->GetClock(),
                                      crypto_config_options_));
  } while(false);

  dispatcher_.reset(CreateQuicDispatcher(ngx_module_context,
                                         create_ngx_timer,
                                         add_ngx_timer,
                                         del_ngx_timer,
                                         free_ngx_timer));
  dispatcher_->InitializeWithWriter(CreateWriter(fd_));
}

QuicPacketWriter* QuicNgxHttpServer::CreateWriter(int fd) {
  // return new QuicDefaultPacketWriter(fd);
  writer_ = new QuicNgxPacketWriter(fd,
                                    set_epoll_out_,
                                    ngx_module_context_);
  return writer_;
}

QuicDispatcher* QuicNgxHttpServer::CreateQuicDispatcher(void* ngx_module_context,
                                                    CreateNgxTimer create_ngx_timer,
                                                    AddNgxTimer add_ngx_timer,
                                                    DelNgxTimer del_ngx_timer,
                                                    FreeNgxTimer free_ngx_timer) {
  return new QuicNgxHttpDispatcher(
      &config_, &crypto_config_, &version_manager_,
      std::unique_ptr<quic::QuicConnectionHelperInterface>(helper_),
      std::unique_ptr<QuicCryptoServerStreamBase::Helper>(
          new QuicSimpleCryptoServerStreamHelper()),
      std::unique_ptr<QuicAlarmFactory>(
         new QuicNgxAlarmFactory(ngx_module_context,
                                 create_ngx_timer,
                                 add_ngx_timer,
                                 del_ngx_timer,
                                 free_ngx_timer)),
      quic_ngx_server_backend_, expected_connection_id_length_);
}

void QuicNgxHttpServer::ReadAndDispatchPackets(void* ngx_connection) {
  quic_ngx_server_backend_->set_ngx_connection(ngx_connection);
  
  dispatcher_->ProcessBufferedChlos(kNumSessionsToCreatePerSocketEvent);

  bool more_to_read = true;
  while (more_to_read) {
    more_to_read = packet_reader_->ReadAndDispatchPackets(
               fd_, port_, clock_, dispatcher_.get(),
               overflow_supported_ ? &packets_dropped_ : nullptr);
  }

  if (dispatcher_->HasChlosBuffered()) {
    dispatcher_->ProcessBufferedChlos(kNumSessionsToCreatePerSocketEvent);
  }
}

bool QuicNgxHttpServer::FlushWriteCache() {
  if (writer_ == nullptr) {
    return false;
  }

  WriteResult r = writer_->Flush();
  return r.status == WRITE_STATUS_BLOCKED;
}

bool QuicNgxHttpServer::CanWrite() {
  dispatcher_->OnCanWrite();
  if (dispatcher_->HasPendingWrites()) {
    return true;
  }

  return FlushWriteCache();
}

void QuicNgxHttpServer::Shutdown() {
  writer_ = nullptr;
  if (!silent_close_) {
    // Before we shut down the epoll server, give all active sessions a chance
    // to notify clients that they're closing.
    dispatcher_->Shutdown();
  }
}

void QuicNgxHttpServer::OnWriteBlocked() {
  set_epoll_out_(ngx_module_context_);
}


}  // namespace quic
