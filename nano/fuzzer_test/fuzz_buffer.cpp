#include <nano/core_test/testutil.hpp>
#include <nano/node/common.hpp>
#include <nano/node/testing.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace nano
{
void force_nano_test_network ();
}
namespace
{
std::shared_ptr<nano::system> system0;
std::shared_ptr<nano::node> node0;

class fuzz_visitor : public nano::message_visitor
{
public:
	virtual void keepalive (nano::keepalive const &) override
	{
	}
	virtual void publish (nano::publish const &) override
	{
	}
	virtual void confirm_req (nano::confirm_req const &) override
	{
	}
	virtual void confirm_ack (nano::confirm_ack const &) override
	{
	}
	virtual void bulk_pull (nano::bulk_pull const &) override
	{
	}
	virtual void bulk_pull_account (nano::bulk_pull_account const &) override
	{
	}
	virtual void bulk_push (nano::bulk_push const &) override
	{
	}
	virtual void frontier_req (nano::frontier_req const &) override
	{
	}
	virtual void node_id_handshake (nano::node_id_handshake const &) override
	{
	}
};
}

/** Fuzz live message parsing. This covers parsing and block/vote uniquing. */
void fuzz_message_parser (const uint8_t * Data, size_t Size)
{
	static bool initialized = false;
	if (!initialized)
	{
		nano::force_nano_test_network ();
		initialized = true;
		system0 = std::make_shared<nano::system> (1);
		node0 = system0->nodes[0];
	}

	fuzz_visitor visitor;
	nano::message_parser parser (node0->block_uniquer, node0->vote_uniquer, visitor, node0->work);
	parser.deserialize_buffer (Data, Size);
}

/** Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput (const uint8_t * Data, size_t Size)
{
	fuzz_message_parser (Data, Size);
	return 0;
}
