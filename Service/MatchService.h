
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio.hpp>

#include <queue>
#include <stdlib.h>

namespace Service
{
    class MatchService 
    {
    public: 
        MatchService();
        ~MatchService();

        bool Init();
        int GetState();
        int SetState(int state);
        void ExecuteQuery();

    private:
        int m_NumberOfQueueThreads;
        int m_NumberOfQueryThreads;
        int m_NumberOfDataThreads;
        
        std::unique_ptr<boost::asio::thread_pool> m_QueueThreads;
        std::unique_ptr<boost::asio::thread_pool> m_QueryThreads;
        std::unique_ptr<boost::asio::thread_pool> m_DataThreads;

        //std::shared_ptr<boost::priority_queue<int>> m_queue; 
        bool IsCapable();
    };
}
