#include <boost/asio.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

#include <coroutine.h>
#include <future.h>

#include <fmt/format.h>

using boost::asio::ip::tcp;

class connection {
public:
  connection(tcp::socket socket) : socket_(std::move(socket)) {}

  gayo::future<boost::asio::const_buffer> read() {
    gayo::promise<boost::asio::const_buffer> pr;
    auto fu = pr.get_future();
    socket_.async_read_some(
        boost::asio::buffer(data, max_length),
        [this, pr = std::move(pr)](boost::system::error_code ec,
                                   std::size_t length) mutable {
          if (!ec) {
            pr.set_value(boost::asio::buffer(data, length));
          } else {
            pr.set_exception(std::make_exception_ptr(
                std::system_error(ec.value(), std::system_category())));
          }
        });
    return fu;
  }

  gayo::future<size_t> write(boost::asio::const_buffer data) {
    gayo::promise<size_t> pr;
    auto fu = pr.get_future();
    boost::asio::async_write(
        socket_, data,
        [pr = std::move(pr)](boost::system::error_code ec,
                             size_t length) mutable {
          if (!ec) {
            pr.set_value(length);
          } else {
            pr.set_exception(std::make_exception_ptr(
                std::system_error(ec.value(), ec.category())));
          }
        });
    return fu;
  }

  tcp::socket socket_;
  enum { max_length = 1024 };
  char data[max_length];
};

class server {
public:
  server(boost::asio::io_service &io_service, short port)
      : socket_(io_service),
        acceptor_(io_service, tcp::endpoint(tcp::v4(), port)) {}

  gayo::future<> start() {
    while (true) {
      auto socket = co_await accept();
      boost::asio::ip::tcp::endpoint remote_ep = socket.remote_endpoint();
      boost::asio::ip::address remote_ad = remote_ep.address();
      std::string s = remote_ad.to_string();
      fmt::print("accept {}\n", s);
      echo(connection(std::move(socket)));
    }
  }

  gayo::future<> echo(connection conn) {
    fmt::print("new connection\n");
    auto b = boost::asio::buffer("hello, I'm server!\n");
    auto size = co_await conn.write(b);

    while (true) {
      auto buffer = co_await conn.read();
      if (buffer.size() == 0) {
        fmt::print("disconnect\n");
        break;
      }
      auto size = co_await conn.write(buffer);
      fmt::print("echo {} bytes\n", size);
    }
  }

private:
  gayo::future<tcp::socket> accept() {
    gayo::promise<tcp::socket> pr;
    auto fu = pr.get_future();
    acceptor_.async_accept(
        socket_,
        [this, pr = std::move(pr)](boost::system::error_code error) mutable {
          if (!error) {
            pr.set_value(std::move(socket_));
          }
        });
    return fu;
  }

  tcp::socket socket_;
  tcp::acceptor acceptor_;
};

int main(int argc, char *argv[]) {
  boost::asio::io_service io_service;

  gayo::sched.hook = [&](std::unique_ptr<gayo::task> task) {
    auto t = task.release();
    io_service.post([t] { t->run_and_dispose(); });
  };

  try {
    server s(io_service, 8000);
    s.start();

    io_service.run();

  } catch (std::exception &ex) {
    std::cerr << ex.what() << std::endl;
  }
  return 0;
}