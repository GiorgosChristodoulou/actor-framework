// This file is part of CAF, the C++ Actor Framework. See the file LICENSE in
// the main distribution directory for license terms and copyright or visit
// https://github.com/actor-framework/actor-framework/blob/master/LICENSE.

#define CAF_SUITE net.datagram_transport

#include "caf/net/datagram_transport.hpp"

#include "caf/test/dsl.hpp"

#include "caf/net/actor_proxy_impl.hpp"
#include "caf/net/endpoint_manager.hpp"
#include "caf/net/endpoint_manager_impl.hpp"
#include "caf/net/ip.hpp"
#include "caf/net/make_endpoint_manager.hpp"
#include "caf/net/multiplexer.hpp"
#include "caf/net/udp_datagram_socket.hpp"

#include "caf/binary_serializer.hpp"
#include "caf/byte_buffer.hpp"
#include "caf/detail/socket_sys_includes.hpp"
#include "caf/make_actor.hpp"
#include "caf/span.hpp"

using namespace caf;
using namespace caf::net;
using namespace caf::net::ip;

namespace {

using byte_buffer_ptr = std::shared_ptr<byte_buffer>;

constexpr string_view hello_manager = "hello manager!";

class dummy_application_factory;

struct fixture : test_coordinator_fixture<> {
  fixture() : shared_buf(std::make_shared<byte_buffer>(1024)) {
    mpx = std::make_shared<multiplexer>();
    if (auto err = mpx->init())
      CAF_FAIL("mpx->init failed: " << err);
    mpx->set_thread_id();
    CAF_CHECK_EQUAL(mpx->num_socket_managers(), 1u);
    auto addresses = local_addresses("localhost");
    CAF_CHECK(!addresses.empty());
    ep = ip_endpoint(*addresses.begin(), 0);
    auto send_pair = unbox(make_udp_datagram_socket(ep));
    send_socket = send_pair.first;
    auto receive_pair = unbox(make_udp_datagram_socket(ep));
    recv_socket = receive_pair.first;
    ep.port(receive_pair.second);
    CAF_MESSAGE("sending message to " << CAF_ARG(ep));
    if (auto err = nonblocking(recv_socket, true))
      CAF_FAIL("nonblocking() returned an error: " << err);
  }

  ~fixture() {
    close(send_socket);
    close(recv_socket);
  }

  bool handle_io_event() override {
    return mpx->poll_once(false);
  }

  error read_from_socket(udp_datagram_socket sock, byte_buffer& buf) {
    uint8_t receive_attempts = 0;
    variant<std::pair<size_t, ip_endpoint>, sec> read_ret;
    do {
      read_ret = read(sock, buf);
      if (auto read_res = get_if<std::pair<size_t, ip_endpoint>>(&read_ret)) {
        buf.resize(read_res->first);
      } else if (get<sec>(read_ret) != sec::unavailable_or_would_block) {
        return make_error(get<sec>(read_ret), "read failed");
      }
      if (++receive_attempts > 100)
        return make_error(sec::runtime_error,
                          "too many unavailable_or_would_blocks");
    } while (read_ret.index() != 0);
    return none;
  }

  multiplexer_ptr mpx;
  byte_buffer_ptr shared_buf;
  ip_endpoint ep;
  udp_datagram_socket send_socket;
  udp_datagram_socket recv_socket;
};

class dummy_application {
public:
  explicit dummy_application(byte_buffer_ptr rec_buf)
    : rec_buf_(std::move(rec_buf)){
      // nop
    };

  template <class Parent>
  error init(Parent&) {
    return none;
  }

  template <class Parent>
  error write_message(Parent& parent,
                      std::unique_ptr<endpoint_manager_queue::message> msg) {
    auto payload_buf = parent.next_payload_buffer();
    binary_serializer sink{parent.system(), payload_buf};
    if (auto err = sink(msg->msg->payload))
      CAF_FAIL("serializing failed: " << err);
    parent.write_packet(payload_buf);
    return none;
  }

  template <class Parent>
  error handle_data(Parent&, span<const byte> data) {
    rec_buf_->clear();
    rec_buf_->insert(rec_buf_->begin(), data.begin(), data.end());
    return none;
  }

  template <class Parent>
  void resolve(Parent& parent, string_view path, const actor& listener) {
    actor_id aid = 42;
    auto uri = unbox(make_uri("test:/id/42"));
    auto nid = make_node_id(uri);
    actor_config cfg;
    endpoint_manager_ptr ptr{&parent.manager()};
    auto p = make_actor<actor_proxy_impl, strong_actor_ptr>(
      aid, nid, &parent.system(), cfg, std::move(ptr));
    anon_send(listener, resolve_atom_v, std::string{path.begin(), path.end()},
              p);
  }

  template <class Parent>
  void new_proxy(Parent&, actor_id) {
    // nop
  }

  template <class Parent>
  void local_actor_down(Parent&, actor_id, error) {
    // nop
  }

  template <class Parent>
  void timeout(Parent&, const std::string&, uint64_t) {
    // nop
  }

  void handle_error(sec sec) {
    CAF_FAIL("handle_error called: " << to_string(sec));
  }

private:
  byte_buffer_ptr rec_buf_;
};

class dummy_application_factory {
public:
  using application_type = dummy_application;

  explicit dummy_application_factory(byte_buffer_ptr buf)
    : buf_(std::move(buf)) {
    // nop
  }

  dummy_application make() {
    return dummy_application{buf_};
  }

private:
  byte_buffer_ptr buf_;
};

} // namespace

CAF_TEST_FIXTURE_SCOPE(datagram_transport_tests, fixture)

CAF_TEST(receive) {
  using transport_type = datagram_transport<dummy_application_factory>;
  if (auto err = nonblocking(recv_socket, true))
    CAF_FAIL("nonblocking() returned an error: " << err);
  auto mgr = make_endpoint_manager(
    mpx, sys,
    transport_type{recv_socket, dummy_application_factory{shared_buf}});
  CAF_CHECK_EQUAL(mgr->init(), none);
  auto mgr_impl = mgr.downcast<endpoint_manager_impl<transport_type>>();
  CAF_CHECK(mgr_impl != nullptr);
  auto& transport = mgr_impl->transport();
  transport.configure_read(net::receive_policy::exactly(hello_manager.size()));
  CAF_CHECK_EQUAL(mpx->num_socket_managers(), 2u);
  CAF_CHECK_EQUAL(write(send_socket, as_bytes(make_span(hello_manager)), ep),
                  hello_manager.size());
  CAF_MESSAGE("wrote " << hello_manager.size() << " bytes.");
  run();
  CAF_CHECK_EQUAL(string_view(reinterpret_cast<char*>(shared_buf->data()),
                              shared_buf->size()),
                  hello_manager);
}

CAF_TEST(resolve and proxy communication) {
  using transport_type = datagram_transport<dummy_application_factory>;
  byte_buffer recv_buf(1024);
  auto uri = unbox(make_uri("test:/id/42"));
  auto mgr = make_endpoint_manager(
    mpx, sys,
    transport_type{send_socket, dummy_application_factory{shared_buf}});
  CAF_CHECK_EQUAL(mgr->init(), none);
  auto mgr_impl = mgr.downcast<endpoint_manager_impl<transport_type>>();
  CAF_CHECK(mgr_impl != nullptr);
  auto& transport = mgr_impl->transport();
  CAF_CHECK_EQUAL(transport.add_new_worker(make_node_id(uri), ep), none);
  run();
  mgr->resolve(uri, self);
  run();
  self->receive(
    [&](resolve_atom, const std::string&, const strong_actor_ptr& p) {
      CAF_MESSAGE("got a proxy, send a message to it");
      self->send(actor_cast<actor>(p), "hello proxy!");
    },
    after(std::chrono::seconds(0)) >>
      [&] { CAF_FAIL("manager did not respond with a proxy."); });
  run();
  CAF_CHECK_EQUAL(read_from_socket(recv_socket, recv_buf), none);
  CAF_MESSAGE("receive buffer contains " << recv_buf.size() << " bytes");
  message msg;
  binary_deserializer source{sys, recv_buf};
  CAF_CHECK_EQUAL(source(msg), none);
  if (msg.match_elements<std::string>())
    CAF_CHECK_EQUAL(msg.get_as<std::string>(0), "hello proxy!");
  else
    CAF_ERROR("expected a string, got: " << to_string(msg));
}

CAF_TEST_FIXTURE_SCOPE_END()
