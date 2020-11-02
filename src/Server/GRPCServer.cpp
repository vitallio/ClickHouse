#include "GRPCServer.h"
#if USE_GRPC

#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Common/CurrentThread.h>
#include <Common/SettingsChanges.h>
#include <DataStreams/AddingDefaultsBlockInputStream.h>
#include <DataStreams/AsynchronousBlockInputStream.h>
#include <Interpreters/Context.h>
#include <Interpreters/InternalTextLogsQueue.h>
#include <Interpreters/executeQuery.h>
#include <IO/ConcatReadBuffer.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <Parsers/parseQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTQueryWithOutput.h>
#include <Parsers/ParserQuery.h>
#include <Processors/Executors/PullingAsyncPipelineExecutor.h>
#include <Server/IServer.h>
#include <Storages/IStorage.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
#include <Poco/Util/LayeredConfiguration.h>
#include <grpc++/server_builder.h>


/// For diagnosing problems use the following environment variables:
/// export GRPC_TRACE=all
/// export GRPC_VERBOSITY=DEBUG

using GRPCService = clickhouse::grpc::ClickHouse::AsyncService;
using GRPCQueryInfo = clickhouse::grpc::QueryInfo;
using GRPCResult = clickhouse::grpc::Result;
using GRPCException = clickhouse::grpc::Exception;
using GRPCProgress = clickhouse::grpc::Progress;

namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_DATABASE;
    extern const int NO_DATA_TO_INSERT;
    extern const int NETWORK_ERROR;
    extern const int INVALID_SESSION_TIMEOUT;
    extern const int INVALID_GRPC_QUERY_INFO;
    extern const int INVALID_CONFIG_PARAMETER;
    extern const int SUPPORT_IS_DISABLED;
}


namespace
{
    grpc_compression_algorithm parseCompressionAlgorithm(const String & str)
    {
        if (str == "none")
            return GRPC_COMPRESS_NONE;
        else if (str == "deflate")
            return GRPC_COMPRESS_DEFLATE;
        else if (str == "gzip")
            return GRPC_COMPRESS_GZIP;
        else if (str == "stream_gzip")
            return GRPC_COMPRESS_STREAM_GZIP;
        else
            throw Exception("Unknown compression algorithm: '" + str + "'", ErrorCodes::INVALID_CONFIG_PARAMETER);
    }

    grpc_compression_level parseCompressionLevel(const String & str)
    {
        if (str == "none")
            return GRPC_COMPRESS_LEVEL_NONE;
        else if (str == "low")
            return GRPC_COMPRESS_LEVEL_LOW;
        else if (str == "medium")
            return GRPC_COMPRESS_LEVEL_MED;
        else if (str == "high")
            return GRPC_COMPRESS_LEVEL_HIGH;
        else
            throw Exception("Unknown compression level: '" + str + "'", ErrorCodes::INVALID_CONFIG_PARAMETER);
    }


    /// Gets file's contents as a string, throws an exception if failed.
    String readFile(const String & filepath)
    {
        Poco::FileInputStream ifs(filepath);
        String res;
        Poco::StreamCopier::copyToString(ifs, res);
        return res;
    }

    /// Makes credentials based on the server config.
    std::shared_ptr<grpc::ServerCredentials> makeCredentials(const Poco::Util::AbstractConfiguration & config)
    {
        if (config.getBool("grpc.enable_ssl", false))
        {
#if USE_SSL
            grpc::SslServerCredentialsOptions options;
            grpc::SslServerCredentialsOptions::PemKeyCertPair key_cert_pair;
            key_cert_pair.private_key = readFile(config.getString("grpc.ssl_key_file"));
            key_cert_pair.cert_chain = readFile(config.getString("grpc.ssl_cert_file"));
            options.pem_key_cert_pairs.emplace_back(std::move(key_cert_pair));
            if (config.getBool("grpc.ssl_require_client_auth", false))
            {
                options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
                if (config.has("grpc.ssl_ca_cert_file"))
                    options.pem_root_certs = readFile(config.getString("grpc.ssl_ca_cert_file"));
            }
            return grpc::SslServerCredentials(options);
#else
            throw DB::Exception(
                "Can't use SSL in grpc, because ClickHouse was built without SSL library",
                DB::ErrorCodes::SUPPORT_IS_DISABLED);
#endif
        }
        return grpc::InsecureServerCredentials();
    }


    /// Gets session's timeout from query info or from the server config.
    std::chrono::steady_clock::duration getSessionTimeout(const GRPCQueryInfo & query_info, const Poco::Util::AbstractConfiguration & config)
    {
        auto session_timeout = query_info.session_timeout();
        if (session_timeout)
        {
            auto max_session_timeout = config.getUInt("max_session_timeout", 3600);
            if (session_timeout > max_session_timeout)
                throw Exception(
                    "Session timeout '" + std::to_string(session_timeout) + "' is larger than max_session_timeout: "
                        + std::to_string(max_session_timeout) + ". Maximum session timeout could be modified in configuration file.",
                    ErrorCodes::INVALID_SESSION_TIMEOUT);
        }
        else
            session_timeout = config.getInt("default_session_timeout", 60);
        return std::chrono::seconds(session_timeout);
    }

    using CompletionCallback = std::function<void(bool)>;

    /// Requests a connection and provides low-level interface for reading and writing.
    class BaseResponder
    {
    public:
        virtual ~BaseResponder() = default;

        virtual void start(GRPCService & grpc_service,
                           grpc::ServerCompletionQueue & new_call_queue,
                           grpc::ServerCompletionQueue & notification_queue,
                           const CompletionCallback & callback) = 0;

        virtual void read(GRPCQueryInfo & query_info_, const CompletionCallback & callback) = 0;
        virtual void write(const GRPCResult & result, const CompletionCallback & callback) = 0;
        virtual void writeAndFinish(const GRPCResult & result, const grpc::Status & status, const CompletionCallback & callback) = 0;

        Poco::Net::SocketAddress getClientAddress() const { String peer = grpc_context.peer(); return Poco::Net::SocketAddress{peer.substr(peer.find(':') + 1)}; }

    protected:
        CompletionCallback * getCallbackPtr(const CompletionCallback & callback)
        {
            /// It would be better to pass callbacks to gRPC calls.
            /// However gRPC calls can be tagged with `void *` tags only.
            /// The map `callbacks` here is used to keep callbacks until they're called.
            size_t callback_id = next_callback_id++;
            auto & callback_in_map = callbacks[callback_id];
            callback_in_map = [this, callback, callback_id](bool ok)
            {
                auto callback_to_call = callback;
                callbacks.erase(callback_id);
                callback_to_call(ok);
            };
            return &callback_in_map;
        }

        grpc::ServerContext grpc_context;

    private:
        grpc::ServerAsyncReaderWriter<GRPCResult, GRPCQueryInfo> reader_writer{&grpc_context};
        std::unordered_map<size_t, CompletionCallback> callbacks;
        size_t next_callback_id = 0;
        /// This class needs no mutex because it's operated from a single thread at any time.
    };

    enum CallType
    {
        CALL_SIMPLE,           /// ExecuteQuery() call
        CALL_STREAMING_INPUT,  /// ExecuteQueryWithStreamingInput() call
        CALL_STREAMING_OUTPUT, /// ExecuteQueryWithStreamingOutput() call
        CALL_STREAMING,        /// ExecuteQueryWithStreaming() call
        CALL_MAX,
    };

    const char * getCallName(CallType call_type)
    {
        switch (call_type)
        {
            case CALL_SIMPLE: return "ExecuteQuery()";
            case CALL_STREAMING_INPUT: return "ExecuteQueryWithStreamingInput()";
            case CALL_STREAMING_OUTPUT: return "ExecuteQueryWithStreamingOutput()";
            case CALL_STREAMING: return "ExecuteQueryWithStreaming()";
            case CALL_MAX: break;
        }
        __builtin_unreachable();
    }

    bool isInputStreaming(CallType call_type)
    {
        return (call_type == CALL_STREAMING_INPUT) || (call_type == CALL_STREAMING);
    }

    bool isOutputStreaming(CallType call_type)
    {
        return (call_type == CALL_STREAMING_OUTPUT) || (call_type == CALL_STREAMING);
    }

    template <enum CallType call_type>
    class Responder;

    template<>
    class Responder<CALL_SIMPLE> : public BaseResponder
    {
    public:
        void start(GRPCService & grpc_service,
                  grpc::ServerCompletionQueue & new_call_queue,
                  grpc::ServerCompletionQueue & notification_queue,
                  const CompletionCallback & callback) override
        {
            grpc_service.RequestExecuteQuery(&grpc_context, &query_info.emplace(), &response_writer, &new_call_queue, &notification_queue, getCallbackPtr(callback));
        }

        void read(GRPCQueryInfo & query_info_, const CompletionCallback & callback) override
        {
            if (!query_info.has_value())
                callback(false);
            query_info_ = std::move(query_info).value();
            query_info.reset();
            callback(true);
        }

        void write(const GRPCResult &, const CompletionCallback &) override
        {
            throw Exception("Responder<CALL_SIMPLE>::write() should not be called", ErrorCodes::LOGICAL_ERROR);
        }

        void writeAndFinish(const GRPCResult & result, const grpc::Status & status, const CompletionCallback & callback) override
        {
            response_writer.Finish(result, status, getCallbackPtr(callback));
        }

    private:
        grpc::ServerAsyncResponseWriter<GRPCResult> response_writer{&grpc_context};
        std::optional<GRPCQueryInfo> query_info;
    };

    template<>
    class Responder<CALL_STREAMING_INPUT> : public BaseResponder
    {
    public:
        void start(GRPCService & grpc_service,
                  grpc::ServerCompletionQueue & new_call_queue,
                  grpc::ServerCompletionQueue & notification_queue,
                  const CompletionCallback & callback) override
        {
            grpc_service.RequestExecuteQueryWithStreamingInput(&grpc_context, &reader, &new_call_queue, &notification_queue, getCallbackPtr(callback));
        }

        void read(GRPCQueryInfo & query_info_, const CompletionCallback & callback) override
        {
            reader.Read(&query_info_, getCallbackPtr(callback));
        }

        void write(const GRPCResult &, const CompletionCallback &) override
        {
            throw Exception("Responder<CALL_STREAMING_INPUT>::write() should not be called", ErrorCodes::LOGICAL_ERROR);
        }

        void writeAndFinish(const GRPCResult & result, const grpc::Status & status, const CompletionCallback & callback) override
        {
            reader.Finish(result, status, getCallbackPtr(callback));
        }

    private:
        grpc::ServerAsyncReader<GRPCResult, GRPCQueryInfo> reader{&grpc_context};
    };

    template<>
    class Responder<CALL_STREAMING_OUTPUT> : public BaseResponder
    {
    public:
        void start(GRPCService & grpc_service,
                  grpc::ServerCompletionQueue & new_call_queue,
                  grpc::ServerCompletionQueue & notification_queue,
                  const CompletionCallback & callback) override
        {
            grpc_service.RequestExecuteQueryWithStreamingOutput(&grpc_context, &query_info.emplace(), &writer, &new_call_queue, &notification_queue, getCallbackPtr(callback));
        }

        void read(GRPCQueryInfo & query_info_, const CompletionCallback & callback) override
        {
            if (!query_info.has_value())
                callback(false);
            query_info_ = std::move(query_info).value();
            query_info.reset();
            callback(true);
        }

        void write(const GRPCResult & result, const CompletionCallback & callback) override
        {
            writer.Write(result, getCallbackPtr(callback));
        }

        void writeAndFinish(const GRPCResult & result, const grpc::Status & status, const CompletionCallback & callback) override
        {
            writer.WriteAndFinish(result, {}, status, getCallbackPtr(callback));
        }

    private:
        grpc::ServerAsyncWriter<GRPCResult> writer{&grpc_context};
        std::optional<GRPCQueryInfo> query_info;
    };

    template<>
    class Responder<CALL_STREAMING> : public BaseResponder
    {
    public:
        void start(GRPCService & grpc_service,
                  grpc::ServerCompletionQueue & new_call_queue,
                  grpc::ServerCompletionQueue & notification_queue,
                  const CompletionCallback & callback) override
        {
            grpc_service.RequestExecuteQueryWithStreaming(&grpc_context, &reader_writer, &new_call_queue, &notification_queue, getCallbackPtr(callback));
        }

        void read(GRPCQueryInfo & query_info_, const CompletionCallback & callback) override
        {
            reader_writer.Read(&query_info_, getCallbackPtr(callback));
        }

        void write(const GRPCResult & result, const CompletionCallback & callback) override
        {
            reader_writer.Write(result, getCallbackPtr(callback));
        }

        void writeAndFinish(const GRPCResult & result, const grpc::Status & status, const CompletionCallback & callback) override
        {
            reader_writer.WriteAndFinish(result, {}, status, getCallbackPtr(callback));
        }

    private:
        grpc::ServerAsyncReaderWriter<GRPCResult, GRPCQueryInfo> reader_writer{&grpc_context};
    };

    std::unique_ptr<BaseResponder> makeResponder(CallType call_type)
    {
        switch(call_type)
        {
            case CALL_SIMPLE: return std::make_unique<Responder<CALL_SIMPLE>>();
            case CALL_STREAMING_INPUT: return std::make_unique<Responder<CALL_STREAMING_INPUT>>();
            case CALL_STREAMING_OUTPUT: return std::make_unique<Responder<CALL_STREAMING_OUTPUT>>();
            case CALL_STREAMING: return std::make_unique<Responder<CALL_STREAMING>>();
            case CALL_MAX: break;
        }
        __builtin_unreachable();
    }


    /// Handles a connection after a responder is started (i.e. after getting a new call).
    class Call
    {
    public:
        Call(CallType call_type_, std::unique_ptr<BaseResponder> responder_, IServer & iserver_, Poco::Logger * log_);
        ~Call();

        void start(const std::function<void(void)> & on_finish_callback);

    private:
        void run();

        void receiveQuery();
        void executeQuery();
        void processInput();
        void generateOutput();
        void generateOutputWithProcessors();
        void finishQuery();
        void onException(const Exception & exception);
        void onFatalError();
        void close();

        void readQueryInfo();
        void startAsyncScanForCancel();
        bool isQueryCancelled();
        void addOutputToResult(const Block & block);
        void addProgressToResult();
        void addTotalsToResult(const Block & totals);
        void addExtremesToResult(const Block & extremes);
        void addLogsToResult();
        void sendResult();
        void throwIfFailedToSendResult();
        void sendFinalResult();
        void sendException(const Exception & exception);

        const CallType call_type;
        std::unique_ptr<BaseResponder> responder;
        IServer & iserver;
        Poco::Logger * log = nullptr;
        InternalTextLogsQueuePtr logs_queue;

        ThreadFromGlobalPool call_thread;
        std::condition_variable signal;
        std::mutex dummy_mutex; /// Doesn't protect anything.

        GRPCQueryInfo query_info;
        size_t query_info_index = static_cast<size_t>(-1);
        GRPCResult result;

        std::shared_ptr<NamedSession> session;
        std::optional<Context> query_context;
        std::optional<CurrentThread::QueryScope> query_scope;
        ASTPtr ast;
        String input_format;
        String output_format;
        uint64_t interactive_delay;
        bool send_exception_with_stacktrace = false;

        std::atomic<bool> failed_to_send_result = false; /// atomic because it can be accessed both from call_thread and queue_thread
        std::atomic<bool> client_want_to_cancel = false; /// atomic because it can be accessed both from call_thread and queue_thread
        bool cancelled = false; /// client want to cancel and we're handling that.

        BlockIO io;
        Progress progress;
    };

    Call::Call(CallType call_type_, std::unique_ptr<BaseResponder> responder_, IServer & iserver_, Poco::Logger * log_)
        : call_type(call_type_), responder(std::move(responder_)), iserver(iserver_), log(log_)
    {
    }

    Call::~Call()
    {
        if (call_thread.joinable())
            call_thread.join();
    }

    void Call::start(const std::function<void(void)> & on_finish_call_callback)
    {
        auto runner_function = [this, on_finish_call_callback]
        {
            try
            {
                run();
            }
            catch (...)
            {
                tryLogCurrentException("GRPCServer");
            }
            on_finish_call_callback();
        };
        call_thread = ThreadFromGlobalPool(runner_function);
    }

    void Call::run()
    {
        try
        {
            receiveQuery();
            executeQuery();
            processInput();
            generateOutput();
            finishQuery();
        }
        catch (Exception & exception)
        {
            onException(exception);
        }
        catch (Poco::Exception & exception)
        {
            onException(Exception{Exception::CreateFromPocoTag{}, exception});
        }
        catch (std::exception & exception)
        {
            onException(Exception{Exception::CreateFromSTDTag{}, exception});
        }
    }

    void Call::receiveQuery()
    {
        LOG_INFO(log, "Handling call {}", getCallName(call_type));

        readQueryInfo();

        auto get_query_text = [&]
        {
            std::string_view query = query_info.query();
            const size_t MAX_QUERY_LENGTH_TO_LOG = 64;
            if (query.length() > MAX_QUERY_LENGTH_TO_LOG)
                query.remove_suffix(query.length() - MAX_QUERY_LENGTH_TO_LOG);
            if (size_t format_pos = query.find(" FORMAT "); format_pos != String::npos)
                query.remove_suffix(query.length() - format_pos - strlen(" FORMAT "));
            if (query == query_info.query())
                return String{query};
            else
                return String{query} + "...";
        };
        LOG_DEBUG(log, "Received initial QueryInfo: query_id: {}, query: {}", query_info.query_id(), get_query_text());
    }

    void Call::executeQuery()
    {
        /// Retrieve user credentials.
        std::string user = query_info.user_name();
        std::string password = query_info.password();
        std::string quota_key = query_info.quota();
        Poco::Net::SocketAddress user_address = responder->getClientAddress();

        if (user.empty())
        {
            user = "default";
            password = "";
        }

        /// Create context.
        query_context.emplace(iserver.context());
        query_scope.emplace(*query_context);

        /// Authentication.
        query_context->setUser(user, password, user_address);
        query_context->setCurrentQueryId(query_info.query_id());
        if (!quota_key.empty())
            query_context->setQuotaKey(quota_key);

        /// The user could specify session identifier and session timeout.
        /// It allows to modify settings, create temporary tables and reuse them in subsequent requests.
        if (!query_info.session_id().empty())
        {
            session = query_context->acquireNamedSession(
                query_info.session_id(), getSessionTimeout(query_info, iserver.config()), query_info.session_check());
            query_context = session->context;
            query_context->setSessionContext(session->context);
        }

        /// Set client info.
        ClientInfo & client_info = query_context->getClientInfo();
        client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
        client_info.interface = ClientInfo::Interface::GRPC;
        client_info.initial_user = client_info.current_user;
        client_info.initial_query_id = client_info.current_query_id;
        client_info.initial_address = client_info.current_address;

        /// Prepare settings.
        SettingsChanges settings_changes;
        for (const auto & [key, value] : query_info.settings())
        {
            settings_changes.push_back({key, value});
        }
        query_context->checkSettingsConstraints(settings_changes);
        query_context->applySettingsChanges(settings_changes);
        const Settings & settings = query_context->getSettingsRef();

        /// Prepare for sending exceptions and logs.
        send_exception_with_stacktrace = query_context->getSettingsRef().calculate_text_stack_trace;
        const auto client_logs_level = query_context->getSettingsRef().send_logs_level;
        if (client_logs_level != LogsLevel::none)
        {
            logs_queue = std::make_shared<InternalTextLogsQueue>();
            logs_queue->max_priority = Poco::Logger::parseLevel(client_logs_level.toString());
            CurrentThread::attachInternalTextLogsQueue(logs_queue, client_logs_level);
            CurrentThread::setFatalErrorCallback([this]{ onFatalError(); });
        }

        /// Set the current database if specified.
        if (!query_info.database().empty())
        {
            if (!DatabaseCatalog::instance().isDatabaseExist(query_info.database()))
                throw Exception("Database " + query_info.database() + " doesn't exist", ErrorCodes::UNKNOWN_DATABASE);
            query_context->setCurrentDatabase(query_info.database());
        }

        /// The interactive delay will be used to show progress.
        interactive_delay = query_context->getSettingsRef().interactive_delay;
        query_context->setProgressCallback([this](const Progress & value) { return progress.incrementPiecewiseAtomically(value); });

        /// Parse the query.
        const char * begin = query_info.query().data();
        const char * end = begin + query_info.query().size();
        ParserQuery parser(end, settings.enable_debug_queries);
        ast = ::DB::parseQuery(parser, begin, end, "", settings.max_query_size, settings.max_parser_depth);

        /// Choose output format.
        query_context->setDefaultFormat(query_info.output_format());
        if (const auto * ast_query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get());
            ast_query_with_output && ast_query_with_output->format)
        {
            output_format = getIdentifierName(ast_query_with_output->format);
        }
        if (output_format.empty())
            output_format = query_context->getDefaultFormat();

        /// Start executing the query.
        auto * insert_query = ast->as<ASTInsertQuery>();
        const auto * query_end = end;
        if (insert_query && insert_query->data)
        {
            query_end = insert_query->data;
        }
        String query(begin, query_end);
        io = ::DB::executeQuery(query, *query_context, false, QueryProcessingStage::Complete, true, true);
    }

    void Call::processInput()
    {
        if (!io.out)
            return;

        auto * insert_query = ast->as<ASTInsertQuery>();
        if (!insert_query)
            throw Exception("Query requires data to insert, but it is not an INSERT query", ErrorCodes::NO_DATA_TO_INSERT);

        if (!insert_query->data && query_info.input_data().empty() && !query_info.use_next_input_data())
            throw Exception("No data to insert", ErrorCodes::NO_DATA_TO_INSERT);

        if (query_info.use_next_input_data() && !isInputStreaming(call_type))
            throw Exception("use_next_input_data is allowed to be set only for streaming input", ErrorCodes::INVALID_GRPC_QUERY_INFO);

        /// Choose input format.
        input_format = insert_query->format;
        if (input_format.empty())
            input_format = "Values";

        /// Prepare read buffer with data to insert.
        ConcatReadBuffer::ReadBuffers buffers;
        std::shared_ptr<ReadBufferFromMemory> insert_query_data_buffer;
        std::shared_ptr<ReadBufferFromMemory> input_data_buffer;
        if (insert_query->data)
        {
            insert_query_data_buffer = std::make_shared<ReadBufferFromMemory>(insert_query->data, insert_query->end - insert_query->data);
            buffers.push_back(insert_query_data_buffer.get());
        }
        if (!query_info.input_data().empty())
        {
            input_data_buffer = std::make_shared<ReadBufferFromMemory>(query_info.input_data().data(), query_info.input_data().size());
            buffers.push_back(input_data_buffer.get());
        }
        auto input_buffer_contacenated = std::make_unique<ConcatReadBuffer>(buffers);
        auto res_stream = query_context->getInputFormat(
            input_format, *input_buffer_contacenated, io.out->getHeader(), query_context->getSettings().max_insert_block_size);

        /// Add default values if necessary.
        auto table_id = query_context->resolveStorageID(insert_query->table_id, Context::ResolveOrdinary);
        if (query_context->getSettingsRef().input_format_defaults_for_omitted_fields && table_id)
        {
            StoragePtr storage = DatabaseCatalog::instance().getTable(table_id, *query_context);
            const auto & columns = storage->getInMemoryMetadataPtr()->getColumns();
            if (!columns.empty())
                res_stream = std::make_shared<AddingDefaultsBlockInputStream>(res_stream, columns, *query_context);
        }

        /// Read input data.
        io.out->writePrefix();

        while (auto block = res_stream->read())
            io.out->write(block);

        while (query_info.use_next_input_data())
        {
            readQueryInfo();
            if (isQueryCancelled())
                break;
            LOG_DEBUG(log, "Received extra QueryInfo with input data: {} bytes", query_info.input_data().size());
            if (!query_info.input_data().empty())
            {
                const char * begin = query_info.input_data().data();
                const char * end = begin + query_info.input_data().size();
                ReadBufferFromMemory data_in(begin, end - begin);
                res_stream = query_context->getInputFormat(
                    input_format, data_in, io.out->getHeader(), query_context->getSettings().max_insert_block_size);

                while (auto block = res_stream->read())
                    io.out->write(block);
            }
        }

        io.out->writeSuffix();
    }

    void Call::generateOutput()
    {
        if (io.pipeline.initialized())
        {
            generateOutputWithProcessors();
            return;
        }

        if (!io.in)
            return;

        AsynchronousBlockInputStream async_in(io.in);
        Stopwatch after_send_progress;

        startAsyncScanForCancel();
        auto check_for_cancel = [&]
        {
            if (isQueryCancelled())
            {
                async_in.cancel(false);
                return true;
            }
            return false;
        };

        async_in.readPrefix();
        while (true)
        {
            Block block;
            if (async_in.poll(interactive_delay / 1000))
            {
                block = async_in.read();
                if (!block)
                    break;
            }

            throwIfFailedToSendResult();
            if (check_for_cancel())
                break;

            if (block && !io.null_format)
                addOutputToResult(block);

            if (after_send_progress.elapsedMicroseconds() >= interactive_delay)
            {
                addProgressToResult();
                after_send_progress.restart();
            }

            addLogsToResult();

            throwIfFailedToSendResult();
            if (check_for_cancel())
                break;

            if (!result.output().empty() || result.has_progress() || result.logs_size())
                sendResult();
        }
        async_in.readSuffix();

        if (!isQueryCancelled())
        {
            addTotalsToResult(io.in->getTotals());
            addExtremesToResult(io.in->getExtremes());
        }
    }

    void Call::generateOutputWithProcessors()
    {
        if (!io.pipeline.initialized())
            return;

        auto executor = std::make_shared<PullingAsyncPipelineExecutor>(io.pipeline);
        Stopwatch after_send_progress;

        startAsyncScanForCancel();
        auto check_for_cancel = [&]
        {
            if (isQueryCancelled())
            {
                executor->cancel();
                return true;
            }
            return false;
        };

        Block block;
        while (executor->pull(block, interactive_delay / 1000))
        {
            throwIfFailedToSendResult();
            if (check_for_cancel())
                break;

            if (block && !io.null_format)
                addOutputToResult(block);

            if (after_send_progress.elapsedMicroseconds() >= interactive_delay)
            {
                addProgressToResult();
                after_send_progress.restart();
            }

            addLogsToResult();

            throwIfFailedToSendResult();
            if (check_for_cancel())
                break;

            if (!result.output().empty() || result.has_progress() || result.logs_size())
                sendResult();
        }

        if (!isQueryCancelled())
        {
            addTotalsToResult(executor->getTotalsBlock());
            addExtremesToResult(executor->getExtremesBlock());
        }
    }

    void Call::finishQuery()
    {
        throwIfFailedToSendResult();
        io.onFinish();
        addProgressToResult();
        query_scope->logPeakMemoryUsage();
        addLogsToResult();
        throwIfFailedToSendResult();
        sendFinalResult();
        close();
        LOG_INFO(log, "Finished call {}", getCallName(call_type));
    }

    void Call::onException(const Exception & exception)
    {
        io.onException();

        LOG_ERROR(log, "Code: {}, e.displayText() = {}, Stack trace:\n\n{}", exception.code(), exception.displayText(), exception.getStackTraceString());

        if (responder)
        {
            try
            {
                /// Try to send logs to client, but it could be risky too.
                addLogsToResult();
            }
            catch (...)
            {
                LOG_WARNING(log, "Couldn't send logs to client");
            }

            try
            {
                sendException(exception);
            }
            catch (...)
            {
                LOG_WARNING(log, "Couldn't send exception information to the client");
            }
        }

        close();
    }

    void Call::onFatalError()
    {
        if (!responder)
            return;
        try
        {
            addLogsToResult();
            sendFinalResult();
        }
        catch (...)
        {
        }
    }

    void Call::close()
    {
        responder.reset();
        io = {};
        query_scope.reset();
        query_context.reset();
        if (session)
            session->release();
        session.reset();
    }

    void Call::readQueryInfo()
    {
        bool ok = false;
        bool completed = false;

        responder->read(query_info, [&](bool ok_)
        {
            /// Called on queue_thread.
            ok = ok_;
            completed = true;
            signal.notify_one();
        });

        std::unique_lock lock{dummy_mutex};
        signal.wait(lock, [&] { return completed; });

        if (!ok)
        {
            if (query_info_index == static_cast<size_t>(-1))
                throw Exception("Failed to read initial QueryInfo", ErrorCodes::NETWORK_ERROR);
            else
                throw Exception("Failed to read extra QueryInfo with input data", ErrorCodes::NETWORK_ERROR);
        }

        ++query_info_index;
        if (query_info.cancel())
            client_want_to_cancel = true;
    }

    void Call::startAsyncScanForCancel()
    {
        /// If input is not streaming then we cannot scan input for the cancel flag.
        if (!isInputStreaming(call_type))
            return;

        responder->read(query_info, [this](bool ok)
        {
            if (ok && query_info.cancel())
                client_want_to_cancel = true;
        });
    }

    bool Call::isQueryCancelled()
    {
        if (cancelled)
            return true;

        if (client_want_to_cancel)
        {
            LOG_INFO(log, "Query cancelled");
            cancelled = true;
            result.set_cancelled(true);
            return true;
        }

        return false;
    }

    void Call::addOutputToResult(const Block & block)
    {
        /// AppendModeTag is necessary because we need to accumulate output if streaming output is disabled.
        WriteBufferFromString buf{*result.mutable_output(), WriteBufferFromString::AppendModeTag{}};
        auto stream = query_context->getOutputFormat(output_format, buf, block);
        stream->write(block);
    }

    void Call::addProgressToResult()
    {
        auto values = progress.fetchAndResetPiecewiseAtomically();
        if (!values.read_rows && !values.read_bytes && !values.total_rows_to_read && !values.written_rows && !values.written_bytes)
            return;
        auto & grpc_progress = *result.mutable_progress();
        /// Sum is used because we need to accumulate values for the case if streaming output is disabled.
        grpc_progress.set_read_rows(grpc_progress.read_rows() + values.read_rows);
        grpc_progress.set_read_bytes(grpc_progress.read_bytes() + values.read_bytes);
        grpc_progress.set_total_rows_to_read(grpc_progress.total_rows_to_read() + values.total_rows_to_read);
        grpc_progress.set_written_rows(grpc_progress.written_rows() + values.written_rows);
        grpc_progress.set_written_bytes(grpc_progress.written_bytes() + values.written_bytes);
    }

    void Call::addTotalsToResult(const Block & totals)
    {
        if (!totals)
            return;

        WriteBufferFromString buf{*result.mutable_totals()};
        auto stream = query_context->getOutputFormat(output_format, buf, totals);
        stream->write(totals);
    }

    void Call::addExtremesToResult(const Block & extremes)
    {
        if (!extremes)
            return;

        WriteBufferFromString buf{*result.mutable_extremes()};
        auto stream = query_context->getOutputFormat(output_format, buf, extremes);
        stream->write(extremes);
    }

    void Call::addLogsToResult()
    {
        if (!logs_queue)
            return;

        static_assert(::clickhouse::grpc::LogEntry_Priority_FATAL       == static_cast<int>(Poco::Message::PRIO_FATAL));
        static_assert(::clickhouse::grpc::LogEntry_Priority_CRITICAL    == static_cast<int>(Poco::Message::PRIO_CRITICAL));
        static_assert(::clickhouse::grpc::LogEntry_Priority_ERROR       == static_cast<int>(Poco::Message::PRIO_ERROR));
        static_assert(::clickhouse::grpc::LogEntry_Priority_WARNING     == static_cast<int>(Poco::Message::PRIO_WARNING));
        static_assert(::clickhouse::grpc::LogEntry_Priority_NOTICE      == static_cast<int>(Poco::Message::PRIO_NOTICE));
        static_assert(::clickhouse::grpc::LogEntry_Priority_INFORMATION == static_cast<int>(Poco::Message::PRIO_INFORMATION));
        static_assert(::clickhouse::grpc::LogEntry_Priority_DEBUG       == static_cast<int>(Poco::Message::PRIO_DEBUG));
        static_assert(::clickhouse::grpc::LogEntry_Priority_TRACE       == static_cast<int>(Poco::Message::PRIO_TRACE));

        MutableColumns columns;
        while (logs_queue->tryPop(columns))
        {
            if (columns.empty() || columns[0]->empty())
                continue;

            size_t col = 0;
            const auto & column_event_time = typeid_cast<const ColumnUInt32 &>(*columns[col++]);
            const auto & column_event_time_microseconds = typeid_cast<const ColumnUInt32 &>(*columns[col++]);
            const auto & column_host_name = typeid_cast<const ColumnString &>(*columns[col++]);
            const auto & column_query_id = typeid_cast<const ColumnString &>(*columns[col++]);
            const auto & column_thread_id = typeid_cast<const ColumnUInt64 &>(*columns[col++]);
            const auto & column_priority = typeid_cast<const ColumnInt8 &>(*columns[col++]);
            const auto & column_source = typeid_cast<const ColumnString &>(*columns[col++]);
            const auto & column_text = typeid_cast<const ColumnString &>(*columns[col++]);
            size_t num_rows = column_event_time.size();

            for (size_t row = 0; row != num_rows; ++row)
            {
                auto & log_entry = *result.add_logs();
                log_entry.set_event_time(column_event_time.getElement(row));
                log_entry.set_event_time_microseconds(column_event_time_microseconds.getElement(row));
                StringRef host_name = column_host_name.getDataAt(row);
                log_entry.set_host_name(host_name.data, host_name.size);
                StringRef query_id = column_query_id.getDataAt(row);
                log_entry.set_query_id(query_id.data, query_id.size);
                log_entry.set_thread_id(column_thread_id.getElement(row));
                log_entry.set_priority(static_cast<::clickhouse::grpc::LogEntry_Priority>(column_priority.getElement(row)));
                StringRef source = column_source.getDataAt(row);
                log_entry.set_source(source.data, source.size);
                StringRef text = column_text.getDataAt(row);
                log_entry.set_text(text.data, text.size);
            }
        }
    }

    void Call::sendResult()
    {
        /// If output is not streaming then only the final result can be sent.
        if (!isOutputStreaming(call_type))
            return;

        /// Send intermediate result without waiting.
        LOG_DEBUG(log, "Sending intermediate result to the client");

        responder->write(result, [this](bool ok)
        {
            if (!ok)
                failed_to_send_result = true;
        });

        /// gRPC has already retrieved all data from `result`, so we don't have to keep it.
        result.Clear();
    }

    void Call::throwIfFailedToSendResult()
    {
        if (failed_to_send_result)
            throw Exception("Failed to send result to the client", ErrorCodes::NETWORK_ERROR);
    }

    void Call::sendFinalResult()
    {
        /// Send final result and wait until it's actually sent.
        LOG_DEBUG(log, "Sending final result to the client");
        bool completed = false;

        responder->writeAndFinish(result, {}, [&](bool ok)
        {
            /// Called on queue_thread.
            if (!ok)
                failed_to_send_result = true;
            completed = true;
            signal.notify_one();
        });

        result.Clear();
        std::unique_lock lock{dummy_mutex};
        signal.wait(lock, [&] { return completed; });

        throwIfFailedToSendResult();
        LOG_TRACE(log, "Final result has been sent to the client");
    }

    void Call::sendException(const Exception & exception)
    {
        auto & grpc_exception = *result.mutable_exception();
        grpc_exception.set_code(exception.code());
        grpc_exception.set_name(exception.name());
        grpc_exception.set_display_text(exception.displayText());
        if (send_exception_with_stacktrace)
            grpc_exception.set_stack_trace(exception.getStackTraceString());
        sendFinalResult();
    }
}


class GRPCServer::Runner
{
public:
    Runner(GRPCServer & owner_) : owner(owner_) {}

    ~Runner()
    {
        if (queue_thread.joinable())
            queue_thread.join();
    }

    void start()
    {
        startReceivingNewCalls();

        /// We run queue in a separate thread.
        auto runner_function = [this]
        {
            try
            {
                run();
            }
            catch (...)
            {
                tryLogCurrentException("GRPCServer");
            }
        };
        queue_thread = ThreadFromGlobalPool{runner_function};
    }

    void stop() { stopReceivingNewCalls(); }

    size_t getNumCurrentCalls() const
    {
        std::lock_guard lock{mutex};
        return current_calls.size();
    }

private:
    void startReceivingNewCalls()
    {
        std::lock_guard lock{mutex};
        responders_for_new_calls.resize(CALL_MAX);
        for (CallType call_type : ext::range(CALL_MAX))
            makeResponderForNewCall(call_type);
    }

    void makeResponderForNewCall(CallType call_type)
    {
        /// `mutex` is already locked.
        responders_for_new_calls[call_type] = makeResponder(call_type);

        responders_for_new_calls[call_type]->start(
            owner.grpc_service, *owner.queue, *owner.queue,
            [this, call_type](bool ok) { onNewCall(call_type, ok); });
    }

    void stopReceivingNewCalls()
    {
        std::lock_guard lock{mutex};
        should_stop = true;
        responders_for_new_calls.clear();
    }

    void onNewCall(CallType call_type, bool responder_started_ok)
    {
         std::lock_guard lock{mutex};
         if (should_stop)
             return;
         auto responder = std::move(responders_for_new_calls[call_type]);
         makeResponderForNewCall(call_type);
         if (responder_started_ok)
         {
             /// Connection established and the responder has been started.
             /// So we pass this responder to a Call and make another responder for next connection.
             auto new_call = std::make_unique<Call>(call_type, std::move(responder), owner.iserver, owner.log);
             auto new_call_ptr = new_call.get();
             current_calls[new_call_ptr] = std::move(new_call);
             new_call_ptr->start([this, new_call_ptr]() { onFinishCall(new_call_ptr); });
         }
    }

    void onFinishCall(Call * call)
    {
        /// Called on call_thread. That's why we can't destroy the `call` right now
        /// (thread can't join to itself). Thus here we only move the `call` from
        /// `current_call` to `finished_calls` and run() will actually destroy the `call`.
        std::lock_guard lock{mutex};
        auto it = current_calls.find(call);
        finished_calls.push_back(std::move(it->second));
        current_calls.erase(it);
    }

    void run()
    {
        while (true)
        {
            {
                std::lock_guard lock{mutex};
                finished_calls.clear(); /// Destroy finished calls.

                /// Continue processing until there is no unfinished calls.
                if (should_stop && current_calls.empty())
                    break;
            }

            bool ok = false;
            void * tag = nullptr;
            if (!owner.queue->Next(&tag, &ok))
            {
                /// Queue shutted down.
                break;
            }

            auto & callback = *static_cast<CompletionCallback *>(tag);
            callback(ok);
        }
    }

    GRPCServer & owner;
    ThreadFromGlobalPool queue_thread;
    std::vector<std::unique_ptr<BaseResponder>> responders_for_new_calls;
    std::map<Call *, std::unique_ptr<Call>> current_calls;
    std::vector<std::unique_ptr<Call>> finished_calls;
    bool should_stop = false;
    mutable std::mutex mutex;
};


GRPCServer::GRPCServer(IServer & iserver_, const Poco::Net::SocketAddress & address_to_listen_)
    : iserver(iserver_), address_to_listen(address_to_listen_), log(&Poco::Logger::get("GRPCServer"))
{}

GRPCServer::~GRPCServer()
{
    runner.reset();

    /// Server should be shutdown before CompletionQueue.
    if (grpc_server)
        grpc_server->Shutdown();

    if (queue)
        queue->Shutdown();
}

void GRPCServer::start()
{
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address_to_listen.toString(), makeCredentials(iserver.config()));
    builder.RegisterService(&grpc_service);
    builder.SetMaxSendMessageSize(iserver.config().getInt("grpc.max_send_message_size", -1));
    builder.SetMaxReceiveMessageSize(iserver.config().getInt("grpc.max_receive_message_size", -1));
    builder.SetDefaultCompressionAlgorithm(parseCompressionAlgorithm(iserver.config().getString("grpc.compression", "none")));
    builder.SetDefaultCompressionLevel(parseCompressionLevel(iserver.config().getString("grpc.compression_level", "none")));

    queue = builder.AddCompletionQueue();
    grpc_server = builder.BuildAndStart();
    runner = std::make_unique<Runner>(*this);
    runner->start();
}


void GRPCServer::stop()
{
    /// Stop receiving new calls.
    runner->stop();
}

size_t GRPCServer::currentConnections() const
{
    return runner->getNumCurrentCalls();
}

}
#endif
