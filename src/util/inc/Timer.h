#pragma once

#include <chrono>

using namespace std;

class Timer
{
    public:
        Timer(){start_time = chrono::high_resolution_clock::now();}
        
        double elapsed()
        {
            return chrono::duration<double>(
                chrono::high_resolution_clock::now() - start_time
            ).count();
        }

        void reset()
        {
            start_time = chrono::high_resolution_clock::now();
        }
        
    private:
        chrono::high_resolution_clock::time_point start_time;

};