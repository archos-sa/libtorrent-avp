#include "test.hpp"
#include "setup_transfer.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/io.hpp"
#include <cstring>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <boost/bind.hpp>

using namespace libtorrent;

int read_message(stream_socket& s, char* buffer)
{
	using namespace libtorrent::detail;
	asio::error_code ec;
	asio::read(s, asio::buffer(buffer, 4), asio::transfer_all(), ec);
	TEST_CHECK(!ec);
	char* ptr = buffer;
	int length = read_int32(ptr);

	asio::read(s, asio::buffer(buffer, length), asio::transfer_all(), ec);
	TEST_CHECK(!ec);
	return length;
}

char const* message_name[] = {"choke", "unchoke", "interested", "not_interested"
	, "have", "bitfield", "request", "piece", "cancel", "dht_port", "", "", ""
	, "suggest_piece", "have_all", "have_none", "reject_request", "allowed_fast"};

void send_allow_fast(stream_socket& s, int piece)
{
	using namespace libtorrent::detail;
	char msg[] = "\0\0\0\x05\x11\0\0\0\0";
	char* ptr = msg + 5;
	write_int32(piece, ptr);
	asio::error_code ec;
	asio::write(s, asio::buffer(msg, 9), asio::transfer_all(), ec);
	TEST_CHECK(!ec);
}

void send_suggest_piece(stream_socket& s, int piece)
{
	using namespace libtorrent::detail;
	char msg[] = "\0\0\0\x05\x0d\0\0\0\0";
	char* ptr = msg + 5;
	write_int32(piece, ptr);
	asio::error_code ec;
	asio::write(s, asio::buffer(msg, 9), asio::transfer_all(), ec);
	TEST_CHECK(!ec);
}

void send_unchoke(stream_socket& s)
{
	char msg[] = "\0\0\0\x01\x01";
	asio::error_code ec;
	asio::write(s, asio::buffer(msg, 5), asio::transfer_all(), ec);
	TEST_CHECK(!ec);
}

void do_handshake(stream_socket& s, sha1_hash const& ih, char* buffer)
{
	char handshake[] = "\x13" "BitTorrent protocol\0\0\0\0\0\0\0\x04"
		"                    " // space for info-hash
		"aaaaaaaaaaaaaaaaaaaa" // peer-id
		"\0\0\0\x01\x0e"; // have_all
	asio::error_code ec;
	std::memcpy(handshake + 28, ih.begin(), 20);
	asio::write(s, asio::buffer(handshake, sizeof(handshake) - 1), asio::transfer_all(), ec);
	TEST_CHECK(!ec);

	// read handshake
	asio::read(s, asio::buffer(buffer, 68), asio::transfer_all(), ec);
	TEST_CHECK(!ec);

	TEST_CHECK(buffer[0] == 19);
	TEST_CHECK(std::memcmp(buffer + 1, "BitTorrent protocol", 19) == 0);

	char* extensions = buffer + 20;
	// check for fast extension support
	TEST_CHECK(extensions[7] & 0x4);
	
#ifndef TORRENT_DISABLE_EXTENSIONS
	// check for extension protocol support
	TEST_CHECK(extensions[5] & 0x10);
#endif
	
#ifndef TORRENT_DISABLE_DHT
	// check for DHT support
	TEST_CHECK(extensions[7] & 0x1);
#endif
	
	TEST_CHECK(std::memcmp(buffer + 28, ih.begin(), 20) == 0);
}

// makes sure that pieces that are allowed and then
// rejected aren't requested again
void test_reject_fast()
{
	boost::intrusive_ptr<torrent_info> t = create_torrent();
	sha1_hash ih = t->info_hash();
	session ses1(fingerprint("LT", 0, 1, 0, 0), std::make_pair(48900, 49000));
	ses1.add_torrent(t, "./tmp1");

	test_sleep(2000);

	io_service ios;
	stream_socket s(ios);
	s.connect(tcp::endpoint(address::from_string("127.0.0.1"), ses1.listen_port()));

	char recv_buffer[1000];
	do_handshake(s, ih, recv_buffer);
	
	std::vector<int> allowed_fast;
	allowed_fast.push_back(0);
	allowed_fast.push_back(1);
	allowed_fast.push_back(2);
	allowed_fast.push_back(3);

	std::for_each(allowed_fast.begin(), allowed_fast.end()
		, bind(&send_allow_fast, boost::ref(s), _1));

	while (!allowed_fast.empty())
	{
		read_message(s, recv_buffer);
		int msg = recv_buffer[0];
		if (msg >= 0 && msg < int(sizeof(message_name)/sizeof(message_name[0])))
			std::cerr << message_name[msg] << std::endl;
		else
			std::cerr << msg << std::endl;
		if (recv_buffer[0] != 0x6) continue;

		using namespace libtorrent::detail;
		char* ptr = recv_buffer + 1;
		int piece = read_int32(ptr);

		std::vector<int>::iterator i = std::find(allowed_fast.begin()
			, allowed_fast.end(), piece);
		TEST_CHECK(i != allowed_fast.end());
		if (i != allowed_fast.end())
			allowed_fast.erase(i);
		// send reject request
		recv_buffer[0] = 0x10;
		asio::error_code ec;
		asio::write(s, asio::buffer("\0\0\0\x0d", 4), asio::transfer_all(), ec);
		TEST_CHECK(!ec);
		asio::write(s, asio::buffer(recv_buffer, 13), asio::transfer_all(), ec);
		TEST_CHECK(!ec);
	}
}

void test_respect_suggest()
{
	boost::intrusive_ptr<torrent_info> t = create_torrent();
	sha1_hash ih = t->info_hash();
	session ses1(fingerprint("LT", 0, 1, 0, 0), std::make_pair(48900, 49000));
	ses1.add_torrent(t, "./tmp1");

	test_sleep(2000);

	io_service ios;
	stream_socket s(ios);
	s.connect(tcp::endpoint(address::from_string("127.0.0.1"), ses1.listen_port()));

	char recv_buffer[1000];
	do_handshake(s, ih, recv_buffer);
	
	std::vector<int> suggested;
	suggested.push_back(0);
	suggested.push_back(1);
	suggested.push_back(2);
	suggested.push_back(3);

	std::for_each(suggested.begin(), suggested.end()
		, bind(&send_suggest_piece, boost::ref(s), _1));

	send_unchoke(s);

	int fail_counter = 100;	
	while (!suggested.empty() && fail_counter > 0)
	{
		read_message(s, recv_buffer);
		std::cerr << "msg: ";
		int msg = recv_buffer[0];
		if (msg >= 0 && msg < int(sizeof(message_name)/sizeof(message_name[0])))
			std::cerr << message_name[msg] << std::endl;
		else
			std::cerr << msg << std::endl;
		fail_counter--;
		if (recv_buffer[0] != 0x6) continue;

		using namespace libtorrent::detail;
		char* ptr = recv_buffer + 1;
		int piece = read_int32(ptr);

		std::vector<int>::iterator i = std::find(suggested.begin()
			, suggested.end(), piece);
		TEST_CHECK(i != suggested.end());
		if (i != suggested.end())
			suggested.erase(i);
		// send reject request
		recv_buffer[0] = 0x10;
		asio::error_code ec;
		asio::write(s, asio::buffer("\0\0\0\x0d", 4), asio::transfer_all(), ec);
		TEST_CHECK(!ec);
		asio::write(s, asio::buffer(recv_buffer, 13), asio::transfer_all(), ec);
		TEST_CHECK(!ec);
	}
	TEST_CHECK(fail_counter > 0);
}

int test_main()
{
	test_reject_fast();
	test_respect_suggest();
	return 0;
}

