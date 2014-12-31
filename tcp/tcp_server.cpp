#include "tcp_server.h"
#include <boost/bind.hpp>

BrinK::tcp::server::server() :
io_service_pos_(0),
port_(0),
acceptor_io_service_(nullptr),
acceptor_(nullptr),
acceptor_work_(nullptr),
acceptor_thread_(nullptr),
// ����Ĭ�ϵ�server��Ϊ�������Ҫ�ⲿ����������start��������
recv_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ���յ����ݣ�������Է������ݵ��ͻ��˻�������߼�������e����Ҫ���ģ�sΪ���յ����ݴ�С��mΪԪ���ݣ�c�Ǹ�client
    // Ĭ���ȷ������ݣ��ڶ�4���ֽڣ������ó�ʱ�����룩
    async_write(c, m);
    async_read(c, 4, 3000);
}),
accept_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ���֣�client��ʼ����ͨ��c->get_param���Եõ�����unique_id������"ip��ַ:�˿�"��e��sͨ������Ҫ����
    // Ĭ�϶�4���ֽ�
    async_read(c, 4, 2000);
}),
timeout_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ��ʱ��m����Ҫ���ģ�eΪ�����룬һ��Ϊ995��sΪ��ʱ����
    // Ĭ�Ϲر�socket
    c->free();
}),
send_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ������ɣ�mΪ������ɵ���Ϣ��e���ο���sΪ��Ϣ��С
    // std::cout << "Sended : " << m << std::endl;
}),
error_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ������ʱ�Ѿ����ո�client������Ҫ�ر�client���ͷŵ��κβ�����eΪ����ԭ�������ɲ��ο�
    // std::cout << "Error : " << e << std::endl;
})
{
    shut_down_ = false;

    started_ = false;

    for (size_t i = 0; i < std::thread::hardware_concurrency(); i++)
        io_services_.emplace_back(std::make_shared < boost::asio::io_service >());

    clients_pool_ = std::make_shared< pool::pool < tcp_client_sptr_t > >([this]
    {
        return tcp_client_sptr_t(std::make_shared < tcp::socket >(this->get_io_service()));
    }
    );
}

BrinK::tcp::server::~server()
{

}

void BrinK::tcp::server::start(const unsigned int& port,
    const complete_handler_t& recv_complete,
    const complete_handler_t& send_complete,
    const complete_handler_t& accept_complete,
    const complete_handler_t& error_handler,
    const complete_handler_t& timeout_handler)
{
    std::unique_lock < std::mutex > lock(mutex_);

    if (started_)
        return;

    if (recv_complete)
        recv_handler_ = recv_complete;

    if (send_complete)
        send_handler_ = send_complete;

    if (accept_complete)
        accept_handler_ = accept_complete;

    if (error_handler)
        error_handler_ = error_handler;

    if (timeout_handler)
        timeout_handler_ = timeout_handler;

    port_ = port;

    shut_down_ = false;

    start_();

    started_ = true;
}

void BrinK::tcp::server::start_()
{
    acceptor_io_service_ = std::make_shared < boost::asio::io_service >();

    acceptor_work_ = std::make_shared < boost::asio::io_service::work >(*acceptor_io_service_);

    acceptor_ = std::make_shared < boost::asio::ip::tcp::acceptor >(*acceptor_io_service_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port_));

    acceptor_thread_ = std::make_shared < std::thread >([this]{ boost::system::error_code ec; this->acceptor_io_service_->run(ec); });

    std::for_each(io_services_.begin(), io_services_.end(), [this](io_service_sptr_t io)
    {
        io_service_works_.emplace_back(std::make_shared < boost::asio::io_service::work >(*io));
        io_service_threads_.emplace_back(std::make_shared < std::thread >([io]{ boost::system::error_code ec; io->run(ec); }));
    });

    accept_clients_();
}

void BrinK::tcp::server::stop()
{
    std::unique_lock < std::mutex > lock(mutex_);

    if (!started_)
        return;

    shut_down_ = true;

    stop_();

    started_ = false;
}

void BrinK::tcp::server::stop_()
{
    boost::system::error_code ec;

    // ȡ�������˿ڣ���io_service�⿪�󶨣����δ���ֵ�socket�����һ��995
    acceptor_work_.reset();
    acceptor_->close(ec);
    acceptor_thread_->join();
    acceptor_io_service_->reset();
    acceptor_.reset();
    acceptor_io_service_.reset();
    acceptor_thread_.reset();

    // �ر����пͻ��ˣ�����ȡ��timer���ֱ𶼻�ͨ��recv�յ�error��Ϣ
    clients_pool_->each([this](tcp_client_sptr_t& client)
    {
        client->free();
    });

    // �ȴ�����io���
    io_service_works_.clear();
    std::for_each(io_service_threads_.begin(), io_service_threads_.end(), [](thread_sptr_t& td){ td->join(); });
    std::for_each(io_services_.begin(), io_services_.end(), [](io_service_sptr_t& io){ io->reset(); });
    io_service_threads_.clear();
}

void BrinK::tcp::server::broadcast(const std::string& msg)
{
    std::unique_lock < std::mutex > lock(mutex_);

    if ((shut_down_) || (!started_))
        return;

    clients_pool_->each([this, msg](tcp_client_sptr_t& client)
    {
        if (client->raw_socket().is_open())
            client->async_write(msg,
            std::bind(&server::handle_write,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3,
            std::placeholders::_4));
    }
    );
}

void BrinK::tcp::server::async_read(tcp_client_sptr_t client, const unsigned int& expect_size, const unsigned __int64& timeout_millseconds)
{
    client->async_read(expect_size,
        std::bind(&server::handle_read,
        this,
        std::placeholders::_1,
        std::placeholders::_2,
        std::placeholders::_3,
        std::placeholders::_4
        )
        );

    client->async_timeout(timeout_millseconds,
        std::bind(&server::handle_timeout,
        this, std::placeholders::_1,
        std::placeholders::_2,
        std::placeholders::_3,
        std::placeholders::_4)
        );
}

void BrinK::tcp::server::async_write(tcp_client_sptr_t client, const std::string& data)
{
    client->async_write(data,
        std::bind(&server::handle_write,
        this,
        std::placeholders::_1,
        std::placeholders::_2,
        std::placeholders::_3,
        std::placeholders::_4));
}

unsigned int BrinK::tcp::server::get_port() const
{
    return port_;
}

void BrinK::tcp::server::accept_clients_()
{
    if (shut_down_)
        return;

    tcp_client_sptr_t& client = clients_pool_->get([this](tcp_client_sptr_t& client)
    {
        client->reset();

        this->acceptor_->async_accept(client->raw_socket(),
            boost::bind(&server::handle_accept,
            this,
            client,
            boost::asio::placeholders::error));
    });
}

void BrinK::tcp::server::handle_accept(tcp_client_sptr_t client, const boost::system::error_code& error)
{
    // socketδ����֮ǰ���赥������֮�����д�����recv����
    if (error)
        handle_error(client, error, 0, "");
    else
    {
        client->accept();
        accept_handler_(boost::any_cast <tcp_client_sptr_t> (client), "", error, 0);
    }

    accept_clients_();
}

boost::asio::io_service& BrinK::tcp::server::get_io_service()
{
    boost::asio::io_service& io_service = *io_services_[io_service_pos_];

    ++io_service_pos_;

    if (io_service_pos_ >= io_services_.size())
        io_service_pos_ = 0;

    return io_service;
}

void BrinK::tcp::server::handle_error(const boost::any& client,
    const boost::system::error_code& error,
    const size_t& bytes_transferred,
    const std::string& buff)
{
    clients_pool_->free(boost::any_cast <tcp_client_sptr_t> (client), [](tcp_client_sptr_t& c){c->free(); });
    error_handler_(boost::any_cast <tcp_client_sptr_t> (client), buff, error, 0);
}

void BrinK::tcp::server::handle_read(const boost::any& client,
    const boost::system::error_code& error,
    const size_t& bytes_transferred,
    const std::string& buff)
{
    // �κε��쳣���󣬶��ᵽhandle_read�ﴦ������������������ݣ����Ҹ���������
    if (error)
        handle_error(client, error, bytes_transferred, buff);
    else
        recv_handler_(boost::any_cast <tcp_client_sptr_t> (client), buff, error, bytes_transferred);
}

void BrinK::tcp::server::handle_timeout(const boost::any& client,
    const boost::system::error_code& error,
    const size_t& timeout_count,
    const std::string& buff)
{
    timeout_handler_(boost::any_cast <tcp_client_sptr_t> (client), buff, error, timeout_count);
}

void BrinK::tcp::server::handle_write(const boost::any& client,
    const boost::system::error_code& error,
    const size_t& bytes_transferred,
    const std::string& buff)
{
    // send���۳�����񣬲���������߼���������쳣����ֹ��Դ������ͳһ����recv����
    send_handler_(boost::any_cast <tcp_client_sptr_t> (client), buff, error, bytes_transferred);
}
