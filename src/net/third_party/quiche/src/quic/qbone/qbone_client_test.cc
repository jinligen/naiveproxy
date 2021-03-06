// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Sets up a dispatcher and sends requests via the QboneClient.

#include "net/third_party/quiche/src/quic/qbone/qbone_client.h"

#include "net/third_party/quiche/src/quic/core/quic_alarm_factory.h"
#include "net/third_party/quiche/src/quic/core/quic_default_packet_writer.h"
#include "net/third_party/quiche/src/quic/core/quic_dispatcher.h"
#include "net/third_party/quiche/src/quic/core/quic_epoll_alarm_factory.h"
#include "net/third_party/quiche/src/quic/core/quic_epoll_connection_helper.h"
#include "net/third_party/quiche/src/quic/core/quic_packet_reader.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_mutex.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_port_utils.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_socket_address.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_test.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_test_loopback.h"
#include "net/quic/platform/impl/quic_socket_utils.h"
#include "net/third_party/quiche/src/quic/qbone/qbone_constants.h"
#include "net/third_party/quiche/src/quic/qbone/qbone_packet_processor_test_tools.h"
#include "net/third_party/quiche/src/quic/qbone/qbone_server_session.h"
#include "net/third_party/quiche/src/quic/test_tools/crypto_test_utils.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_server_peer.h"
#include "net/third_party/quiche/src/quic/test_tools/server_thread.h"
#include "net/third_party/quiche/src/quic/tools/quic_memory_cache_backend.h"
#include "net/third_party/quiche/src/quic/tools/quic_server.h"

namespace quic {
namespace test {
namespace {

string TestPacketIn(const string& body) {
  return PrependIPv6HeaderForTest(body, 5);
}

string TestPacketOut(const string& body) {
  return PrependIPv6HeaderForTest(body, 4);
}

class DataSavingQbonePacketWriter : public QbonePacketWriter {
 public:
  void WritePacketToNetwork(const char* packet, size_t size) override {
    QuicWriterMutexLock lock(&mu_);
    data_.push_back(string(packet, size));
  }

  std::vector<string> data() {
    QuicWriterMutexLock lock(&mu_);
    return data_;
  }

 private:
  QuicMutex mu_;
  std::vector<string> data_;
};

// A subclass of a qbone session that will own the connection passed in.
class ConnectionOwningQboneServerSession : public QboneServerSession {
 public:
  ConnectionOwningQboneServerSession(
      const ParsedQuicVersionVector& supported_versions,
      QuicConnection* connection,
      Visitor* owner,
      const QuicConfig& config,
      const QuicCryptoServerConfig* quic_crypto_server_config,
      QuicCompressedCertsCache* compressed_certs_cache,
      QbonePacketWriter* writer)
      : QboneServerSession(supported_versions,
                           connection,
                           owner,
                           config,
                           quic_crypto_server_config,
                           compressed_certs_cache,
                           writer,
                           TestLoopback6(),
                           TestLoopback6(),
                           64,
                           nullptr),
        connection_(connection) {}

 private:
  // Note that we don't expect the QboneServerSession or any of its parent
  // classes to do anything with the connection_ in their destructors.
  std::unique_ptr<QuicConnection> connection_;
};

class QuicQboneDispatcher : public QuicDispatcher {
 public:
  QuicQboneDispatcher(
      const QuicConfig* config,
      const QuicCryptoServerConfig* crypto_config,
      QuicVersionManager* version_manager,
      std::unique_ptr<QuicConnectionHelperInterface> helper,
      std::unique_ptr<QuicCryptoServerStream::Helper> session_helper,
      std::unique_ptr<QuicAlarmFactory> alarm_factory,
      QbonePacketWriter* writer)
      : QuicDispatcher(config,
                       crypto_config,
                       version_manager,
                       std::move(helper),
                       std::move(session_helper),
                       std::move(alarm_factory),
                       kQuicDefaultConnectionIdLength),
        writer_(writer) {}

  QuicSession* CreateQuicSession(
      QuicConnectionId id,
      const QuicSocketAddress& client,
      QuicStringPiece alpn,
      const quic::ParsedQuicVersion& version) override {
    CHECK_EQ(alpn, "qbone");
    QuicConnection* connection =
        new QuicConnection(id, client, helper(), alarm_factory(), writer(),
                           /* owns_writer= */ false, Perspective::IS_SERVER,
                           ParsedQuicVersionVector{version});
    // The connection owning wrapper owns the connection created.
    QboneServerSession* session = new ConnectionOwningQboneServerSession(
        GetSupportedVersions(), connection, this, config(), crypto_config(),
        compressed_certs_cache(), writer_);
    session->Initialize();
    return session;
  }

  QuicConnectionId GenerateNewServerConnectionId(
      ParsedQuicVersion version,
      QuicConnectionId connection_id) const override {
    char connection_id_bytes[kQuicDefaultConnectionIdLength] = {};
    return QuicConnectionId(connection_id_bytes, sizeof(connection_id_bytes));
  }

 private:
  QbonePacketWriter* writer_;
};

class QboneTestServer : public QuicServer {
 public:
  explicit QboneTestServer(std::unique_ptr<ProofSource> proof_source)
      : QuicServer(std::move(proof_source), &response_cache_) {}
  QuicDispatcher* CreateQuicDispatcher() override {
    QuicEpollAlarmFactory alarm_factory(epoll_server());
    return new QuicQboneDispatcher(
        &config(), &crypto_config(), version_manager(),
        std::unique_ptr<QuicEpollConnectionHelper>(
            new QuicEpollConnectionHelper(epoll_server(),
                                          QuicAllocator::BUFFER_POOL)),
        std::unique_ptr<QuicCryptoServerStream::Helper>(
            new QboneCryptoServerStreamHelper()),
        std::unique_ptr<QuicEpollAlarmFactory>(
            new QuicEpollAlarmFactory(epoll_server())),
        &writer_);
  }

  std::vector<string> data() { return writer_.data(); }

  void WaitForDataSize(int n) {
    while (data().size() != n) {
    }
  }

 private:
  quic::QuicMemoryCacheBackend response_cache_;
  DataSavingQbonePacketWriter writer_;
};

class QboneTestClient : public QboneClient {
 public:
  QboneTestClient(QuicSocketAddress server_address,
                  const QuicServerId& server_id,
                  const ParsedQuicVersionVector& supported_versions,
                  QuicEpollServer* epoll_server,
                  std::unique_ptr<ProofVerifier> proof_verifier)
      : QboneClient(server_address,
                    server_id,
                    supported_versions,
                    /*session_owner=*/nullptr,
                    QuicConfig(),
                    epoll_server,
                    std::move(proof_verifier),
                    &qbone_writer_,
                    nullptr) {}

  ~QboneTestClient() override {}

  void SendData(const string& data) {
    qbone_session()->ProcessPacketFromNetwork(data);
  }

  void WaitForWriteToFlush() {
    while (connected() && session()->HasDataToWrite()) {
      WaitForEvents();
    }
  }

  void WaitForDataSize(int n) {
    while (data().size() != n) {
      WaitForEvents();
    }
  }

  std::vector<string> data() { return qbone_writer_.data(); }

 private:
  DataSavingQbonePacketWriter qbone_writer_;
};

TEST(QboneClientTest, SendDataFromClient) {
  auto server = new QboneTestServer(crypto_test_utils::ProofSourceForTesting());
  QuicSocketAddress server_address(TestLoopback(),
                                   QuicPickServerPortForTestsOrDie());
  ServerThread server_thread(server, server_address);
  server_thread.Initialize();
  server_thread.Start();

  QuicEpollServer epoll_server;
  QboneTestClient client(
      server_address,
      QuicServerId("test.example.com", server_address.port(), false),
      AllSupportedVersions(), &epoll_server,
      crypto_test_utils::ProofVerifierForTesting());
  ASSERT_TRUE(client.Initialize());
  ASSERT_TRUE(client.Connect());
  ASSERT_TRUE(client.WaitForCryptoHandshakeConfirmed());
  client.SendData(TestPacketIn("hello"));
  client.SendData(TestPacketIn("world"));
  client.WaitForWriteToFlush();
  server->WaitForDataSize(2);
  EXPECT_THAT(server->data()[0], testing::Eq(TestPacketOut("hello")));
  EXPECT_THAT(server->data()[1], testing::Eq(TestPacketOut("world")));
  auto server_session =
      static_cast<QboneServerSession*>(QuicServerPeer::GetDispatcher(server)
                                           ->session_map()
                                           .begin()
                                           ->second.get());
  string long_data(QboneConstants::kMaxQbonePacketBytes - sizeof(ip6_hdr) - 1,
                   'A');
  // Pretend the server gets data.
  server_thread.Schedule([&server_session, &long_data]() {
    server_session->ProcessPacketFromNetwork(
        TestPacketIn("Somethingsomething"));
    server_session->ProcessPacketFromNetwork(TestPacketIn(long_data));
    server_session->ProcessPacketFromNetwork(TestPacketIn(long_data));
  });
  client.WaitForDataSize(3);
  EXPECT_THAT(client.data()[0],
              testing::Eq(TestPacketOut("Somethingsomething")));
  EXPECT_THAT(client.data()[1], testing::Eq(TestPacketOut(long_data)));
  EXPECT_THAT(client.data()[2], testing::Eq(TestPacketOut(long_data)));

  client.Disconnect();
  server_thread.Quit();
  server_thread.Join();
}

}  // namespace
}  // namespace test
}  // namespace quic
