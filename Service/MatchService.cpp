#include "MatchService.h"
#include "ShenNongImp.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "MatchService.grpc.pb.h"

using namespace Service;
using grpc::ServerContext;
using grpc::Status;
using grpc::Server;
using grpc::ServerBuilder;

MatchService::MatchService()
{
}

MatchService::~MatchService()
{
}

bool MatchService::Init()
{
    m_QueueThreads = std::make_unique<ThreadPool>(m_NumberOfQueueThreads);
    m_QueryThreads = std::make_unique<ThreadPool>(m_NumberOfQueryThreads);
    m_DataThreads  = std::make_unique<ThreadPool>(m_NumberOfDataThreads);

    std::string server_address = "0.0.0.0:60060";
    ShenNongImp service;

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    server->Wait();

    return true;
}

int MatchService::GetState()
{
    return -1;
}

int MatchService::SetState(int state)
{
    return -1;
}

void MatchService::ExecuteQuery()
{
    if (!IsCapable()) return;

    /*
    * Post the query execution as a work item to the query thread pool.
    * The lambda captures nothing — real implementation would capture
    * query context and invoke the ISR/executor pipeline.
    */
    m_QueryThreads->post([]() {
        /*
        * Query execution pipeline runs here.
        * Real implementation invokes ISR tree and executor.
        */
    });
}
