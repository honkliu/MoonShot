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

    QueueThreads.Start();
    QueryThreads.Start();
    DataThreads.Start();
    
    return true;
}

void OnQueryReceived()
{
    if (isCapable())
    {
        SmartTokenize();
        CreateQueryPlans()
        m_queue.Enqueue(queryJob)
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