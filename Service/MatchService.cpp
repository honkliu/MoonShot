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

    m_QueueThreads.reset(new boost::asio::thread_pool(m_NumberOfQueueThreads));
    m_QueryThreads.reset(new boost::asio::thread_pool(m_NumberOfQueryThreads));
    m_DataThreads.reset(new boost::asio::thread_pool(m_NumberOfQueueThreads));
    
    for (int i = 0; )
    m_QueueThreads.get(),  

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