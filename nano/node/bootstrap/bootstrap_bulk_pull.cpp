#include <nano/node/bootstrap/bootstrap.hpp>
#include <nano/node/bootstrap/bootstrap_bulk_pull.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>

nano::pull_info::pull_info (nano::account const & account_a, nano::block_hash const & head_a, nano::block_hash const & end_a, count_t count_a) :
account (account_a),
head (head_a),
head_original (head_a),
end (end_a),
count (count_a)
{
}

nano::bulk_pull_client::bulk_pull_client (std::shared_ptr<nano::bootstrap_client> connection_a, nano::pull_info const & pull_a) :
connection (connection_a),
known_account (0),
pull (pull_a),
pull_blocks (0),
unexpected_count (0)
{
	connection->attempt->condition.notify_all ();
}

nano::bulk_pull_client::~bulk_pull_client ()
{
	// If received end block is not expected end block
	if (expected != pull.end)
	{
		pull.head = expected;
		if (connection->attempt->mode != nano::bootstrap_mode::legacy)
		{
			pull.account = expected;
		}
		pull.processed += pull_blocks - unexpected_count;
		connection->attempt->requeue_pull (pull);
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Bulk pull end block is not expected %1% for account %2%") % pull.end.to_string () % pull.account.to_account ()));
		}
	}
	else
	{
		connection->node->bootstrap_initiator.cache.remove (pull);
	}
	{
		nano::lock_guard<std::mutex> mutex (connection->attempt->mutex);
		--connection->attempt->pulling;
	}
	connection->attempt->condition.notify_all ();
}

void nano::bulk_pull_client::request ()
{
	expected = pull.head;
	nano::bulk_pull req;
	req.start = (pull.head == pull.head_original) ? pull.account : pull.head; // Account for new pulls, head for cached pulls
	req.end = pull.end;
	req.count = pull.count;
	req.set_count_present (pull.count != 0);

	if (connection->node->config.logging.bulk_pull_logging ())
	{
		nano::unique_lock<std::mutex> lock (connection->attempt->mutex);
		connection->node->logger.try_log (boost::str (boost::format ("Requesting account %1% from %2%. %3% accounts in queue") % pull.account.to_account () % connection->channel->to_string () % connection->attempt->pulls.size ()));
	}
	else if (connection->node->config.logging.network_logging () && connection->attempt->should_log ())
	{
		nano::unique_lock<std::mutex> lock (connection->attempt->mutex);
		connection->node->logger.always_log (boost::str (boost::format ("%1% accounts in pull queue") % connection->attempt->pulls.size ()));
	}
	auto this_l (shared_from_this ());
	connection->channel->send (
	req, [this_l](boost::system::error_code const & ec, size_t size_a) {
		if (!ec)
		{
			this_l->throttled_receive_block ();
		}
		else
		{
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error sending bulk pull request to %1%: to %2%") % ec.message () % this_l->connection->channel->to_string ()));
			}
			this_l->connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_request_failure, nano::stat::dir::in);
		}
	},
	false); // is bootstrap traffic is_droppable false
}

void nano::bulk_pull_client::throttled_receive_block ()
{
	if (!connection->node->block_processor.half_full ())
	{
		receive_block ();
	}
	else
	{
		auto this_l (shared_from_this ());
		connection->node->alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (1), [this_l]() {
			if (!this_l->connection->pending_stop && !this_l->connection->attempt->stopped)
			{
				this_l->throttled_receive_block ();
			}
		});
	}
}

void nano::bulk_pull_client::receive_block ()
{
	auto this_l (shared_from_this ());
	connection->channel->socket->async_read (connection->receive_buffer, 1, [this_l](boost::system::error_code const & ec, size_t size_a) {
		if (!ec)
		{
			this_l->received_type ();
		}
		else
		{
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error receiving block type: %1%") % ec.message ()));
			}
			this_l->connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_receive_block_failure, nano::stat::dir::in);
		}
	});
}

void nano::bulk_pull_client::received_type ()
{
	auto this_l (shared_from_this ());
	nano::block_type type (static_cast<nano::block_type> (connection->receive_buffer->data ()[0]));
	switch (type)
	{
		case nano::block_type::send:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::send_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::receive:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::receive_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::open:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::open_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::change:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::change_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::state:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::state_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::not_a_block:
		{
			// Avoid re-using slow peers, or peers that sent the wrong blocks.
			if (!connection->pending_stop && expected == pull.end)
			{
				connection->attempt->pool_connection (connection);
			}
			break;
		}
		default:
		{
			if (connection->node->config.logging.network_packet_logging ())
			{
				connection->node->logger.try_log (boost::str (boost::format ("Unknown type received as block type: %1%") % static_cast<int> (type)));
			}
			break;
		}
	}
}

void nano::bulk_pull_client::received_block (boost::system::error_code const & ec, size_t size_a, nano::block_type type_a)
{
	if (!ec)
	{
		nano::bufferstream stream (connection->receive_buffer->data (), size_a);
		std::shared_ptr<nano::block> block (nano::deserialize_block (stream, type_a));
		if (block != nullptr && !nano::work_validate (*block))
		{
			auto hash (block->hash ());
			if (connection->node->config.logging.bulk_pull_logging ())
			{
				std::string block_l;
				block->serialize_json (block_l, connection->node->config.logging.single_line_record ());
				connection->node->logger.try_log (boost::str (boost::format ("Pulled block %1% %2%") % hash.to_string () % block_l));
			}
			// Is block expected?
			bool block_expected (false);
			if (hash == expected)
			{
				expected = block->previous ();
				block_expected = true;
			}
			else
			{
				unexpected_count++;
			}
			if (pull_blocks == 0 && block_expected)
			{
				known_account = block->account ();
			}
			if (connection->block_count++ == 0)
			{
				connection->start_time = std::chrono::steady_clock::now ();
			}
			connection->attempt->total_blocks++;
			bool stop_pull (connection->attempt->process_block (block, known_account, pull_blocks, block_expected));
			pull_blocks++;
			if (!stop_pull && !connection->hard_stop.load ())
			{
				/* Process block in lazy pull if not stopped
				Stop usual pull request with unexpected block & more than 16k blocks processed
				to prevent spam */
				if (connection->attempt->mode != nano::bootstrap_mode::legacy || unexpected_count < 16384)
				{
					throttled_receive_block ();
				}
			}
			else if (stop_pull && block_expected)
			{
				expected = pull.end;
				connection->attempt->pool_connection (connection);
			}
		}
		else
		{
			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log ("Error deserializing block received from pull request");
			}
			connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_deserialize_receive_block, nano::stat::dir::in);
		}
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Error bulk receiving block: %1%") % ec.message ()));
		}
		connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_receive_block_failure, nano::stat::dir::in);
	}
}

nano::bulk_pull_account_client::bulk_pull_account_client (std::shared_ptr<nano::bootstrap_client> connection_a, nano::account const & account_a) :
connection (connection_a),
account (account_a),
pull_blocks (0)
{
	connection->attempt->condition.notify_all ();
}

nano::bulk_pull_account_client::~bulk_pull_account_client ()
{
	{
		nano::lock_guard<std::mutex> mutex (connection->attempt->mutex);
		--connection->attempt->pulling;
	}
	connection->attempt->condition.notify_all ();
}

void nano::bulk_pull_account_client::request ()
{
	nano::bulk_pull_account req;
	req.account = account;
	req.minimum_amount = connection->node->config.receive_minimum;
	req.flags = nano::bulk_pull_account_flags::pending_hash_and_amount;
	if (connection->node->config.logging.bulk_pull_logging ())
	{
		nano::unique_lock<std::mutex> lock (connection->attempt->mutex);
		connection->node->logger.try_log (boost::str (boost::format ("Requesting pending for account %1% from %2%. %3% accounts in queue") % req.account.to_account () % connection->channel->to_string () % connection->attempt->wallet_accounts.size ()));
	}
	else if (connection->node->config.logging.network_logging () && connection->attempt->should_log ())
	{
		nano::unique_lock<std::mutex> lock (connection->attempt->mutex);
		connection->node->logger.always_log (boost::str (boost::format ("%1% accounts in pull queue") % connection->attempt->wallet_accounts.size ()));
	}
	auto this_l (shared_from_this ());
	connection->channel->send (
	req, [this_l](boost::system::error_code const & ec, size_t size_a) {
		if (!ec)
		{
			this_l->receive_pending ();
		}
		else
		{
			this_l->connection->attempt->requeue_pending (this_l->account);
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error starting bulk pull request to %1%: to %2%") % ec.message () % this_l->connection->channel->to_string ()));
			}
			this_l->connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_error_starting_request, nano::stat::dir::in);
		}
	},
	false); // is bootstrap traffic is_droppable false
}

void nano::bulk_pull_account_client::receive_pending ()
{
	auto this_l (shared_from_this ());
	size_t size_l (sizeof (nano::uint256_union) + sizeof (nano::uint128_union));
	connection->channel->socket->async_read (connection->receive_buffer, size_l, [this_l, size_l](boost::system::error_code const & ec, size_t size_a) {
		// An issue with asio is that sometimes, instead of reporting a bad file descriptor during disconnect,
		// we simply get a size of 0.
		if (size_a == size_l)
		{
			if (!ec)
			{
				nano::block_hash pending;
				nano::bufferstream frontier_stream (this_l->connection->receive_buffer->data (), sizeof (nano::uint256_union));
				auto error1 (nano::try_read (frontier_stream, pending));
				(void)error1;
				assert (!error1);
				nano::amount balance;
				nano::bufferstream balance_stream (this_l->connection->receive_buffer->data () + sizeof (nano::uint256_union), sizeof (nano::uint128_union));
				auto error2 (nano::try_read (balance_stream, balance));
				(void)error2;
				assert (!error2);
				if (this_l->pull_blocks == 0 || !pending.is_zero ())
				{
					if (this_l->pull_blocks == 0 || balance.number () >= this_l->connection->node->config.receive_minimum.number ())
					{
						this_l->pull_blocks++;
						{
							if (!pending.is_zero ())
							{
								auto transaction (this_l->connection->node->store.tx_begin_read ());
								if (!this_l->connection->node->store.block_exists (transaction, pending))
								{
									this_l->connection->attempt->lazy_start (pending);
								}
							}
						}
						this_l->receive_pending ();
					}
					else
					{
						this_l->connection->attempt->requeue_pending (this_l->account);
					}
				}
				else
				{
					this_l->connection->attempt->pool_connection (this_l->connection);
				}
			}
			else
			{
				this_l->connection->attempt->requeue_pending (this_l->account);
				if (this_l->connection->node->config.logging.network_logging ())
				{
					this_l->connection->node->logger.try_log (boost::str (boost::format ("Error while receiving bulk pull account frontier %1%") % ec.message ()));
				}
			}
		}
		else
		{
			this_l->connection->attempt->requeue_pending (this_l->account);
			if (this_l->connection->node->config.logging.network_message_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Invalid size: expected %1%, got %2%") % size_l % size_a));
			}
		}
	});
}

/**
 * Handle a request for the pull of all blocks associated with an account
 * The account is supplied as the "start" member, and the final block to
 * send is the "end" member.  The "start" member may also be a block
 * hash, in which case the that hash is used as the start of a chain
 * to send.  To determine if "start" is interpretted as an account or
 * hash, the ledger is checked to see if the block specified exists,
 * if not then it is interpretted as an account.
 *
 * Additionally, if "start" is specified as a block hash the range
 * is inclusive of that block hash, that is the range will be:
 * [start, end); In the case that a block hash is not specified the
 * range will be exclusive of the frontier for that account with
 * a range of (frontier, end)
 */
void nano::bulk_pull_server::set_current_end ()
{
	include_start = false;
	assert (request != nullptr);
	auto transaction (connection->node->store.tx_begin_read ());
	if (!connection->node->store.block_exists (transaction, request->end))
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Bulk pull end block doesn't exist: %1%, sending everything") % request->end.to_string ()));
		}
		request->end.clear ();
	}

	if (connection->node->store.block_exists (transaction, request->start))
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Bulk pull request for block hash: %1%") % request->start.to_string ()));
		}

		current = request->start;
		include_start = true;
	}
	else
	{
		nano::account_info info;
		auto no_address (connection->node->store.account_get (transaction, request->start, info));
		if (no_address)
		{
			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log (boost::str (boost::format ("Request for unknown account: %1%") % request->start.to_account ()));
			}
			current = request->end;
		}
		else
		{
			current = info.head;
			if (!request->end.is_zero ())
			{
				auto account (connection->node->ledger.account (transaction, request->end));
				if (account != request->start)
				{
					if (connection->node->config.logging.bulk_pull_logging ())
					{
						connection->node->logger.try_log (boost::str (boost::format ("Request for block that is not on account chain: %1% not on %2%") % request->end.to_string () % request->start.to_account ()));
					}
					current = request->end;
				}
			}
		}
	}

	sent_count = 0;
	if (request->is_count_present ())
	{
		max_count = request->count;
	}
	else
	{
		max_count = 0;
	}
}

void nano::bulk_pull_server::send_next ()
{
	auto block (get_next ());
	if (block != nullptr)
	{
		std::vector<uint8_t> send_buffer;
		{
			nano::vectorstream stream (send_buffer);
			nano::serialize_block (stream, *block);
		}
		auto this_l (shared_from_this ());
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Sending block: %1%") % block->hash ().to_string ()));
		}
		connection->socket->async_write (nano::shared_const_buffer (std::move (send_buffer)), [this_l](boost::system::error_code const & ec, size_t size_a) {
			this_l->sent_action (ec, size_a);
		});
	}
	else
	{
		send_finished ();
	}
}

std::shared_ptr<nano::block> nano::bulk_pull_server::get_next ()
{
	std::shared_ptr<nano::block> result;
	bool send_current = false, set_current_to_end = false;

	/*
	 * Determine if we should reply with a block
	 *
	 * If our cursor is on the final block, we should signal that we
	 * are done by returning a null result.
	 *
	 * Unless we are including the "start" member and this is the
	 * start member, then include it anyway.
	 */
	if (current != request->end)
	{
		send_current = true;
	}
	else if (current == request->end && include_start == true)
	{
		send_current = true;

		/*
		 * We also need to ensure that the next time
		 * are invoked that we return a null result
		 */
		set_current_to_end = true;
	}

	/*
	 * Account for how many blocks we have provided.  If this
	 * exceeds the requested maximum, return an empty object
	 * to signal the end of results
	 */
	if (max_count != 0 && sent_count >= max_count)
	{
		send_current = false;
	}

	if (send_current)
	{
		auto transaction (connection->node->store.tx_begin_read ());
		result = connection->node->store.block_get (transaction, current);
		if (result != nullptr && set_current_to_end == false)
		{
			auto previous (result->previous ());
			if (!previous.is_zero ())
			{
				current = previous;
			}
			else
			{
				current = request->end;
			}
		}
		else
		{
			current = request->end;
		}

		sent_count++;
	}

	/*
	 * Once we have processed "get_next()" once our cursor is no longer on
	 * the "start" member, so this flag is not relevant is always false.
	 */
	include_start = false;

	return result;
}

void nano::bulk_pull_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		send_next ();
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Unable to bulk send block: %1%") % ec.message ()));
		}
	}
}

void nano::bulk_pull_server::send_finished ()
{
	nano::shared_const_buffer send_buffer (static_cast<uint8_t> (nano::block_type::not_a_block));
	auto this_l (shared_from_this ());
	if (connection->node->config.logging.bulk_pull_logging ())
	{
		connection->node->logger.try_log ("Bulk sending finished");
	}
	connection->socket->async_write (send_buffer, [this_l](boost::system::error_code const & ec, size_t size_a) {
		this_l->no_block_sent (ec, size_a);
	});
}

void nano::bulk_pull_server::no_block_sent (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		assert (size_a == 1);
		connection->finish_request ();
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log ("Unable to send not-a-block");
		}
	}
}

nano::bulk_pull_server::bulk_pull_server (std::shared_ptr<nano::bootstrap_server> const & connection_a, std::unique_ptr<nano::bulk_pull> request_a) :
connection (connection_a),
request (std::move (request_a))
{
	set_current_end ();
}

/**
 * Bulk pull blocks related to an account
 */
void nano::bulk_pull_account_server::set_params ()
{
	assert (request != nullptr);

	/*
	 * Parse the flags
	 */
	invalid_request = false;
	pending_include_address = false;
	pending_address_only = false;
	if (request->flags == nano::bulk_pull_account_flags::pending_address_only)
	{
		pending_address_only = true;
	}
	else if (request->flags == nano::bulk_pull_account_flags::pending_hash_amount_and_address)
	{
		/**
		 ** This is the same as "pending_hash_and_amount" but with the
		 ** sending address appended, for UI purposes mainly.
		 **/
		pending_include_address = true;
	}
	else if (request->flags == nano::bulk_pull_account_flags::pending_hash_and_amount)
	{
		/** The defaults are set above **/
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Invalid bulk_pull_account flags supplied %1%") % static_cast<uint8_t> (request->flags)));
		}

		invalid_request = true;

		return;
	}

	/*
	 * Initialize the current item from the requested account
	 */
	current_key.account = request->account;
	current_key.hash = 0;
}

void nano::bulk_pull_account_server::send_frontier ()
{
	/*
	 * This function is really the entry point into this class,
	 * so handle the invalid_request case by terminating the
	 * request without any response
	 */
	if (!invalid_request)
	{
		auto stream_transaction (connection->node->store.tx_begin_read ());

		// Get account balance and frontier block hash
		auto account_frontier_hash (connection->node->ledger.latest (stream_transaction, request->account));
		auto account_frontier_balance_int (connection->node->ledger.account_balance (stream_transaction, request->account));
		nano::uint128_union account_frontier_balance (account_frontier_balance_int);

		// Write the frontier block hash and balance into a buffer
		std::vector<uint8_t> send_buffer;
		{
			nano::vectorstream output_stream (send_buffer);
			write (output_stream, account_frontier_hash.bytes);
			write (output_stream, account_frontier_balance.bytes);
		}

		// Send the buffer to the requestor
		auto this_l (shared_from_this ());
		connection->socket->async_write (nano::shared_const_buffer (std::move (send_buffer)), [this_l](boost::system::error_code const & ec, size_t size_a) {
			this_l->sent_action (ec, size_a);
		});
	}
}

void nano::bulk_pull_account_server::send_next_block ()
{
	/*
	 * Get the next item from the queue, it is a tuple with the key (which
	 * contains the account and hash) and data (which contains the amount)
	 */
	auto block_data (get_next ());
	auto block_info_key (block_data.first.get ());
	auto block_info (block_data.second.get ());

	if (block_info_key != nullptr)
	{
		/*
		 * If we have a new item, emit it to the socket
		 */

		std::vector<uint8_t> send_buffer;
		if (pending_address_only)
		{
			nano::vectorstream output_stream (send_buffer);

			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log (boost::str (boost::format ("Sending address: %1%") % block_info->source.to_string ()));
			}

			write (output_stream, block_info->source.bytes);
		}
		else
		{
			nano::vectorstream output_stream (send_buffer);

			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log (boost::str (boost::format ("Sending block: %1%") % block_info_key->hash.to_string ()));
			}

			write (output_stream, block_info_key->hash.bytes);
			write (output_stream, block_info->amount.bytes);

			if (pending_include_address)
			{
				/**
				 ** Write the source address as well, if requested
				 **/
				write (output_stream, block_info->source.bytes);
			}
		}

		auto this_l (shared_from_this ());
		connection->socket->async_write (nano::shared_const_buffer (std::move (send_buffer)), [this_l](boost::system::error_code const & ec, size_t size_a) {
			this_l->sent_action (ec, size_a);
		});
	}
	else
	{
		/*
		 * Otherwise, finalize the connection
		 */
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Done sending blocks")));
		}

		send_finished ();
	}
}

std::pair<std::unique_ptr<nano::pending_key>, std::unique_ptr<nano::pending_info>> nano::bulk_pull_account_server::get_next ()
{
	std::pair<std::unique_ptr<nano::pending_key>, std::unique_ptr<nano::pending_info>> result;

	while (true)
	{
		/*
		 * For each iteration of this loop, establish and then
		 * destroy a database transaction, to avoid locking the
		 * database for a prolonged period.
		 */
		auto stream_transaction (connection->node->store.tx_begin_read ());
		auto stream (connection->node->store.pending_begin (stream_transaction, current_key));

		if (stream == nano::store_iterator<nano::pending_key, nano::pending_info> (nullptr))
		{
			break;
		}

		nano::pending_key key (stream->first);
		nano::pending_info info (stream->second);

		/*
		 * Get the key for the next value, to use in the next call or iteration
		 */
		current_key.account = key.account;
		current_key.hash = key.hash.number () + 1;

		/*
		 * Finish up if the response is for a different account
		 */
		if (key.account != request->account)
		{
			break;
		}

		/*
		 * Skip entries where the amount is less than the requested
		 * minimum
		 */
		if (info.amount < request->minimum_amount)
		{
			continue;
		}

		/*
		 * If the pending_address_only flag is set, de-duplicate the
		 * responses.  The responses are the address of the sender,
		 * so they are are part of the pending table's information
		 * and not key, so we have to de-duplicate them manually.
		 */
		if (pending_address_only)
		{
			if (!deduplication.insert (info.source).second)
			{
				/*
				 * If the deduplication map gets too
				 * large, clear it out.  This may
				 * result in some duplicates getting
				 * sent to the client, but we do not
				 * want to commit too much memory
				 */
				if (deduplication.size () > 4096)
				{
					deduplication.clear ();
				}
				continue;
			}
		}

		result.first = std::unique_ptr<nano::pending_key> (new nano::pending_key (key));
		result.second = std::unique_ptr<nano::pending_info> (new nano::pending_info (info));

		break;
	}

	return result;
}

void nano::bulk_pull_account_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		send_next_block ();
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Unable to bulk send block: %1%") % ec.message ()));
		}
	}
}

void nano::bulk_pull_account_server::send_finished ()
{
	/*
	 * The "bulk_pull_account" final sequence is a final block of all
	 * zeros.  If we are sending only account public keys (with the
	 * "pending_address_only" flag) then it will be 256-bits of zeros,
	 * otherwise it will be either 384-bits of zeros (if the
	 * "pending_include_address" flag is not set) or 640-bits of zeros
	 * (if that flag is set).
	 */
	std::vector<uint8_t> send_buffer;
	{
		nano::vectorstream output_stream (send_buffer);
		nano::uint256_union account_zero (0);
		nano::uint128_union balance_zero (0);

		write (output_stream, account_zero.bytes);

		if (!pending_address_only)
		{
			write (output_stream, balance_zero.bytes);
			if (pending_include_address)
			{
				write (output_stream, account_zero.bytes);
			}
		}
	}

	auto this_l (shared_from_this ());

	if (connection->node->config.logging.bulk_pull_logging ())
	{
		connection->node->logger.try_log ("Bulk sending for an account finished");
	}

	connection->socket->async_write (nano::shared_const_buffer (std::move (send_buffer)), [this_l](boost::system::error_code const & ec, size_t size_a) {
		this_l->complete (ec, size_a);
	});
}

void nano::bulk_pull_account_server::complete (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		if (pending_address_only)
		{
			assert (size_a == 32);
		}
		else
		{
			if (pending_include_address)
			{
				assert (size_a == 80);
			}
			else
			{
				assert (size_a == 48);
			}
		}

		connection->finish_request ();
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log ("Unable to pending-as-zero");
		}
	}
}

nano::bulk_pull_account_server::bulk_pull_account_server (std::shared_ptr<nano::bootstrap_server> const & connection_a, std::unique_ptr<nano::bulk_pull_account> request_a) :
connection (connection_a),
request (std::move (request_a)),
current_key (0, 0)
{
	/*
	 * Setup the streaming response for the first call to "send_frontier" and  "send_next_block"
	 */
	set_params ();
}