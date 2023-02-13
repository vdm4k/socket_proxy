﻿#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/tcp.h>
#include <socket_proxy/libev/libev.h>
#include <socket_proxy/linux/tcp_settings.h>
#include <socket_proxy/linux/tcp_stream.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace jkl::sp::lnx {

tcp_stream::~tcp_stream() { cleanup(); }

bool tcp_stream::get_local_address(jkl::proto::ip::address::version ver, int fd,
                                   jkl::proto::ip::full_address &addr) {
  if (fd > 0) {
    switch (ver) {
      case jkl::proto::ip::address::version::e_v4: {
        struct sockaddr_in t_local_addr = {0, 0, {0}, {0}};
        socklen_t addrlen = sizeof(t_local_addr);
        getsockname(fd, (struct sockaddr *)&t_local_addr, &addrlen);
        addr = jkl::proto::ip::full_address(
            jkl::proto::ip::address(
                jkl::proto::ip::v4::address(t_local_addr.sin_addr.s_addr)),
            htons(t_local_addr.sin_port));
        return true;
      }
      case jkl::proto::ip::address::version::e_v6: {
        sockaddr_in6 t_local_addr = {0, 0, 0, {{{0}}}, 0};
        socklen_t addrlen = sizeof(t_local_addr);
        char addr_buf[50];
        getsockname(fd, (struct sockaddr *)&t_local_addr, &addrlen);
        inet_ntop(AF_INET6, &t_local_addr.sin6_addr, addr_buf,
                  sizeof(addr_buf));
        addr = jkl::proto::ip::full_address(
            jkl::proto::ip::address(jkl::proto::ip::v6::address(addr_buf)),
            htons(t_local_addr.sin6_port));
        return true;
      }
      default:
        break;
    }
  }
  return false;
}

void tcp_stream::set_socket_specific_options() {
  {
    int mode = 1;
    ioctl(_file_descr, FIONBIO, &mode);
    stream_socket_parameters *sparam =
        (stream_socket_parameters *)get_stream_settings();
    if (sparam->_buffer_size) {
      int optval = *sparam->_buffer_size;
#ifdef SO_SNDBUF
      if (-1 == setsockopt(_file_descr, SOL_SOCKET, SO_SNDBUF,
                           reinterpret_cast<char const *>(&optval),
                           sizeof(optval))) {
      }
#endif  // SO_SNDBUF
#ifdef SO_RCVBUF
      if (-1 == setsockopt(_file_descr, SOL_SOCKET, SO_RCVBUF,
                           reinterpret_cast<char const *>(&optval),
                           sizeof(optval))) {
      }
#endif  // SO_RCVBUF
    }
  }
  {
#ifdef TCP_NODELAY
    /* Set the NODELAY option */
    int optval = 1;
    if (-1 == ::setsockopt(_file_descr, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<char const *>(&optval),
                           sizeof(optval))) {
    }
#endif  // TCP_NODELAY
  }
}

bool tcp_stream::create_socket() {
  int rc = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (-1 != rc) {
    _file_descr = rc;
    set_socket_specific_options();

  } else {
    set_detailed_error("couldn't create socket\n");
  }
  return rc != -1;
}

uint32_t find_scope_id(const jkl::proto::ip::v6::address &addr) {
  uint32_t scope_id{0};
  struct ifaddrs *ifap{nullptr}, *ifa{nullptr};
  getifaddrs(&ifap);

  for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
    if (ifa && ifa->ifa_addr && AF_INET6 == ifa->ifa_addr->sa_family) {
      struct sockaddr_in6 *in6 =
          reinterpret_cast<struct sockaddr_in6 *>(ifa->ifa_addr);
      char addr_buf[50];
      inet_ntop(AF_INET6, &in6->sin6_addr, addr_buf, sizeof(addr_buf));
      jkl::proto::ip::v6::address addr_s(addr_buf);
      if (addr == addr_s) {
        scope_id = in6->sin6_scope_id;
        break;
      }
    }
  }

  freeifaddrs(ifap);
  return scope_id;
}

bool tcp_stream::bind_on_address() {
  switch (_self_addr_full.get_address().get_version()) {
    case jkl::proto::ip::address::version::e_v4: {
      sockaddr_in local_addr = {0, 0, {0}, {0}};
      if (0 < fill_sockaddr(_self_addr_full, local_addr)) {
        if (0 == ::bind(_file_descr, reinterpret_cast<sockaddr *>(&local_addr),
                        sizeof(local_addr)))
          return true;
        _detailed_error.append("couldn't bind on address - " +
                               _self_addr_full.to_string() + ", errno - " +
                               strerror(errno) + "\n");
      }
      break;
    }
    case jkl::proto::ip::address::version::e_v6: {
      sockaddr_in6 local_addr = {0, 0, 0, {{{0}}}, 0};
      if (fill_sockaddr(_self_addr_full, (sockaddr_in &)(local_addr)) > 0) {
        if (0 == ::bind(_file_descr, reinterpret_cast<sockaddr *>(&local_addr),
                        sizeof(local_addr)))
          return true;
        _detailed_error.append("couldn't bind on address - " +
                               _self_addr_full.to_string() + ", errno - " +
                               strerror(errno) + "\n");
      }
      break;
    }
    default:
      _detailed_error.append(
          "incorrect self address pass to function bind_on_address\n");
      break;
  }
  return false;
}

bool tcp_stream::fill_sockaddr(jkl::proto::ip::full_address const &ipaddr,
                               sockaddr_in &addr) {
  switch (ipaddr.get_address().get_version()) {
    case jkl::proto::ip::address::version::e_v4: {
      addr = {0, 0, {0}, {0}};
      addr.sin_family = AF_INET;
      memcpy(&addr.sin_addr.s_addr, ipaddr.get_address().get_data(),
             jkl::proto::ip::v4::address::e_bytes_size);
      addr.sin_port = __builtin_bswap16(ipaddr.get_port());
      return true;
    }
    case jkl::proto::ip::address::version::e_v6: {
      uint32_t lscope_id{find_scope_id(ipaddr.get_address().to_v6())};
      if (lscope_id) {
        sockaddr_in6 local_addr = {0, 0, 0, {{{0}}}, 0};
        memset(&local_addr, 0, sizeof(sockaddr_in6));
        local_addr.sin6_family = AF_INET6;
        memcpy(&local_addr.sin6_addr, ipaddr.get_address().get_data(),
               jkl::proto::ip::v6::address::e_bytes_size);
        local_addr.sin6_port = __builtin_bswap16(ipaddr.get_port());
        local_addr.sin6_scope_id = lscope_id;
        auto *p_addr = reinterpret_cast<sockaddr_in6 *>(&addr);
        *p_addr = local_addr;
        return true;
      } else {
        _detailed_error.append("couldn't find scope_id for address - " +
                               ipaddr.to_string());
      }
      break;
    }
    default: {
      _detailed_error.append("incorrect address type\n");
      break;
    }
  }
  return false;
}

void tcp_stream::cleanup() {
  if (-1 != _file_descr) {
    ::close(_file_descr);
    _file_descr = -1;
  }
}

jkl::proto::ip::full_address const &tcp_stream::get_self_address() const {
  return _self_addr_full;
}

std::string const &tcp_stream::get_detailed_error() const {
  return _detailed_error;
}

tcp_stream::state tcp_stream::get_state() const { return _state; }

void tcp_stream::set_state_changed_cb(state_changed_cb cb, std::any user_data) {
  _state_changed_cb = cb;
  _param_state_changed_cb = user_data;
}

void tcp_stream::set_connection_state(state new_state) {
  _state = new_state;
  if (_state_changed_cb) _state_changed_cb(this, _param_state_changed_cb);
}

void tcp_stream::set_detailed_error(const std::string &str) {
  if (errno)
    _detailed_error = str + ", errno - " + strerror(errno);
  else
    _detailed_error = str;
}

}  // namespace jkl::sp::lnx
