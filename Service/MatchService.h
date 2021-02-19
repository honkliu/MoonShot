
/*
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/asio/thread_pool.hpp>
*/
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
//        boost::shared_ptr<prioirty_queue<int>> m_queue; 
    };
}
