#include <boost/asio.hpp>
#include <grpcpp/grpcpp.h>

#include "MatchService.h"
#include "ShenNongImp.h"
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "MatchService.grpc.pb.h"

using namespace Service;
using grpc::ServerContext;
using grpc::Status;
using grpc::Server;
using grpc::ServerBuilder;

/*
using ShenNong::Greeter;
using ShenNong::Question;
using ShenNong::Answers;
*/
/*
class ShenNongImp final: public Greeter::Service {
    Status Query121(ServerContext * context, const Question * question, Answers *answers) override
    {
        return Status::OK;
    }
*/
/*
    Status Query129(ServerContext * context, const Question * question, ServerWriter<Answers>* writer);
    
    Status Query129(ServerContext * context, ServerReader<Question>* reader, Answers* answers);

    Status Query929(ServerContext* context, ServerReader<Question>* reader, ServerWriter<Answers>* writer);

};
*/
MatchService::MatchService()
{

}

MatchService::~MatchService()
{

}

bool MatchService::Init()
{
    /*
    * Bind the query execution to the method.
    * For steps.
    * 1. OnQueryReceived()
    * 2. OnQuery
    * 3. OnQueryExecuted()
    */

    //QueueThreads.Start();
    //QueryThreads.Start();
    //DataThreads.Start();

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

bool IsCapable()
{
    return true;
}

void OnQueryReceived()
{
    if (IsCapable())
    {
        //SmartTokenize();
        //CreateQueryPlans()
        //m_queue.Enqueue(queryJob)
    }
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
    OnQueryReceived();
}