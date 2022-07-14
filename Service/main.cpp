#include <iostream>

using namespace std;

namespace Service {

    void handleCtrlBreak(int signal)
    {
        return;
    }

    bool StartMatchService()
    {
        return true;
    }

}

int main(int argc, char ** argv)
{
    cout << "Service Started" << endl;
    Service::StartMatchService();
}