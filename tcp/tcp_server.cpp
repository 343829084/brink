#include "tcp_server.h"
#include <boost/bind.hpp>

BrinK::tcp::server::server(const unsigned int& default_recv_len,
    const unsigned __int64& default_socket_recv_timeout_millseconds) :
    default_recv_len_(default_recv_len),
    default_socket_recv_timeout_millseconds_(default_socket_recv_timeout_millseconds),
    io_service_pos_(0),
    port_(0),
    acceptor_io_service_(nullptr),
    acceptor_(nullptr),
    acceptor_work_(nullptr),
    acceptor_thread_(nullptr),
    // ����Ĭ�ϵ�server��Ϊ��ʹ��һ���̳߳�����֤ͬ���������Ҫ�ⲿ����������start��������
    recv_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ���յ����ݣ�������Է������ݵ��ͻ��˻�������߼�������e����Ҫ���ģ�sΪ���յ����ݴ�С��mΪԪ���ݣ�c�Ǹ�client
    // std::cout << m << std::endl;

}),
accept_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ���֣�client��ʼ����ͨ��c->get_param���Եõ�����unique_id������"ip��ַ:�˿�"��e��sͨ������Ҫ����

}),
timeout_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ��ʱ��m����Ҫ���ģ�eΪ�����룬һ��Ϊ995��sΪ��ʱ����������Ӧ����ҵ��c->closesocket()
    c->closesocket();
}),
send_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ������ɣ�mΪ������ɵ���Ϣ��e���ο���sΪ��Ϣ��С

}),
error_handler_([this](tcp_client_sptr_t c, const std::string& m, const boost::system::error_code& e, const size_t& s)
{
    // ������ʱ�Ѿ����ո�client������Ҫ�ر�client���ͷŵ��κβ�����eΪ����ԭ�������ɲ��ο�

})
{
    shut_down_ = false;

    started_ = false;

    clients_pool_ = std::make_shared< pool::pool < tcp_client_sptr_t > >([]
    {
        return tcp_client_sptr_t(std::make_shared < tcp::socket > ());
    }
    );
}

BrinK::tcp::server::~server()
{

}

boost::asio::io_service& BrinK::tcp::server::get_io_service()
{
    std::unique_lock < std::mutex > lock(io_services_mutex_);

    boost::asio::io_service& io_service = *io_services_[io_service_pos_];

    ++io_service_pos_;

    if (io_service_pos_ >= io_services_.size())
        io_service_pos_ = 0;

    return io_service;
}

void BrinK::tcp::server::accept_clients()
{
    {
        // ��stop_serverʱ����Ҫͬ�����������
        std::unique_lock < std::mutex > lock(acceptor_mutex_);
        if (shut_down_)
            return;
    }

    tcp_client_sptr_t& client = clients_pool_->get([this](tcp_client_sptr_t& client)
    {
        client->reset(get_io_service());
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
        client->async_read(default_recv_len_,
            std::bind(&server::handle_read,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3,
            std::placeholders::_4),
            default_socket_recv_timeout_millseconds_,
            std::bind(&server::handle_timeout,
            this, std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3,
            std::placeholders::_4)
            );

        thread_pool_.post([this, client, error]
        {
            this->accept_handler_(boost::any_cast < tcp_client_sptr_t > (client),
                "",
                error,
                0);
        });
    }

    accept_clients();
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
    for (size_t i = 0; i < std::thread::hardware_concurrency(); i++)
    {
        std::unique_lock < std::mutex > lock(io_services_mutex_);
        io_services_.emplace_back(std::make_shared < boost::asio::io_service > ());
        works_.emplace_back(std::make_shared < boost::asio::io_service::work > (*io_services_.back()));
        threads_.emplace_back(std::make_shared < std::thread > ([this, i]{ boost::system::error_code ec; this->io_services_[i]->run(ec); }));
    }

    acceptor_io_service_ = std::make_shared < boost::asio::io_service > ();

    acceptor_work_ = std::make_shared < boost::asio::io_service::work > (*acceptor_io_service_);

    acceptor_ = std::make_shared < boost::asio::ip::tcp::acceptor > (*acceptor_io_service_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port_));

    acceptor_thread_ = std::make_shared < std::thread > ([this]{ boost::system::error_code ec; this->acceptor_io_service_->run(ec); });

    thread_pool_.start();

    accept_clients();
}

void BrinK::tcp::server::handle_error(const boost::any& client,
    const boost::system::error_code& error,
    const size_t& bytes_transferred,
    const std::string& buff)
{
    clients_pool_->free(boost::any_cast < tcp_client_sptr_t > (client), [](tcp_client_sptr_t& c){c->free(); });

    thread_pool_.post([this, client, buff, error]
    {
        this->error_handler_(boost::any_cast < tcp_client_sptr_t > (client),
            buff,
            error,
            0);
    });
}

void BrinK::tcp::server::stop()
{
    std::unique_lock < std::mutex > lock(mutex_);

    if (!started_)
        return;

    {
        std::unique_lock < std::mutex > lock(acceptor_mutex_);
        shut_down_ = true;
    }

    stop_();

    started_ = false;
}

void BrinK::tcp::server::stop_()
{
    boost::system::error_code ec;

    // ȡ�������˿ڣ���io_service�⿪�󶨣����δ���ֵ�socket�����һ��995error
    acceptor_work_.reset();
    acceptor_->cancel(ec);
    acceptor_->close(ec);
    acceptor_thread_->join();
    acceptor_io_service_->reset();
    acceptor_.reset();
    acceptor_io_service_.reset();
    acceptor_thread_.reset();

    // �ر����пͻ��ˣ�����ȡ��timer���ֱ𶼻�ͨ��recv�յ�error��Ϣ
    clients_pool_->each([this](tcp_client_sptr_t& client)
    {
        client->closesocket();
    });

    // ���Ĺ�����ȡ��server�����io_serivce
    {
        std::unique_lock < std::mutex > lock(io_services_mutex_);
        works_.clear();
        std::for_each(threads_.begin(), threads_.end(), [](thread_sptr_t& td){ td->join(); });
        std::for_each(io_services_.begin(), io_services_.end(), [](io_service_sptr_t& io){ io->reset(); });
        threads_.clear();

        // �ȴ��̳߳ؽ���
        thread_pool_.wait();
        thread_pool_.stop();

        io_services_.clear();
        io_service_pos_ = 0;
    }
}

unsigned int BrinK::tcp::server::get_port()
{
    return port_;
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
    {
        boost::any_cast < tcp_client_sptr_t >(client)->async_read(default_recv_len_,
            std::bind(&server::handle_read,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3,
            std::placeholders::_4),
            default_socket_recv_timeout_millseconds_,
            std::bind(&server::handle_timeout,
            this,
            std::placeholders::_1,
            std::placeholders::_2,
            std::placeholders::_3,
            std::placeholders::_4));

        thread_pool_.post([this, client, error, buff, bytes_transferred]
        {
            this->recv_handler_(boost::any_cast < tcp_client_sptr_t > (client),
                buff,
                error,
                bytes_transferred);
        });
    }
}

void BrinK::tcp::server::handle_timeout(const boost::any& client,
    const boost::system::error_code& error,
    const size_t& timeout_count,
    const std::string& buff)
{
    thread_pool_.post([this, client, buff, error, timeout_count]
    {
        this->timeout_handler_(boost::any_cast < tcp_client_sptr_t > (client),
            buff,
            error,
            timeout_count);
    });
}

void BrinK::tcp::server::handle_write(const boost::any& client,
    const boost::system::error_code& error,
    const size_t& bytes_transferred,
    const std::string& buff)
{
    // send���۳�����񣬲���������߼���������쳣����ֹ��Դ������ͳһ����recv����
    thread_pool_.post([this, client, buff, error, bytes_transferred]
    {
        this->send_handler_(boost::any_cast < tcp_client_sptr_t > (client),
            buff,
            error,
            bytes_transferred);
    });
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

void BrinK::tcp::server::set_receive_length(const unsigned int& length)
{
    default_recv_len_ = length;
}

void BrinK::tcp::server::set_timeout(const unsigned __int64& milliseconds)
{
    default_socket_recv_timeout_millseconds_ = milliseconds;
}

