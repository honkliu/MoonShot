#include <boost/asio.h>

#include "MatchService.h"

using namespace Service;

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
    ShenNongImp service();

    ServerBuilder builder;

    builder.AddListeningPort(server_address, grpc::InsecureServerlCredetials());
    buileer.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuilderAndStart());

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
    OnQueryReceived()
}