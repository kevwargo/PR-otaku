#define _POSIX_C_SOURCE 200000L
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <pthread.h>
#include <execinfo.h>
#include <signal.h>

#include <time.h>

using namespace std;

#include "mpi.h"

void handleStatusRequest(int id, int thread_id);

enum
{
    REQUEST_TAG = 1,
    STATUS_TAG = 2,
    EXIT_TAG = 3
};

enum
{
    INSIDE_STATE = 1,
    WAITING_STATE,
    OUTSIDE_STATE
};

struct Status
{
    int state; /* waiting, inside or outside */
    bool requesting; /* requested process is still requesting other processes */
    bool hasMetCurrent; /* if requested process has requesting one in his array of wait0ing processes */
    int clock;
} *ProcessStatuses;

bool insideState = false;
bool waitingState = false;
bool requesting = false; 

int MyId;
int LamportClock;

pthread_mutex_t mutex;
pthread_mutex_t recvMutex;
// pthread_mutex_t recvMutex;

vector<int> waiting_for_current_to_exit;

void handler(int sig) {
    void *array[20];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 20);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

double gettime(struct timespec *result)
{
    struct timespec ts;
    struct timespec *tsp = result;
    if (! tsp)
        tsp = &ts;
    clock_gettime(CLOCK_REALTIME, tsp);
    return ((double)tsp->tv_sec + 1e-9 * (double)tsp->tv_nsec);
}

void atomicRecv(void *buf, int count, const MPI::Datatype& datatype,
                int source, int tag, MPI::Status& status, int thread_id)
{
    pthread_mutex_lock(&recvMutex);
    try 
    {
        MPI::COMM_WORLD.Recv(buf, count, datatype, source, tag, status);
    }
    catch (MPI::Exception failure)
    {
        cerr << "ERRORRRRRRRRRRRR: " << failure.Get_error_string() << endl;
        MPI::COMM_WORLD.Abort(1);
    }
    int got_tag = status.Get_tag();
    if (tag != got_tag && tag != MPI::ANY_TAG)
        printf("AAAAAAA!!!!!!!!!!!!!!!!!!!!!!!!! %d:%d %d %d\n", MyId, thread_id, tag, got_tag);
    pthread_mutex_unlock(&recvMutex);
}

void atomicSend(const void *buf, int count, const MPI::Datatype& datatype,
                int dest, int tag)
{
    pthread_mutex_lock(&recvMutex);
    try 
    {
        MPI::COMM_WORLD.Send(buf, count, datatype, dest, tag);
    }
    catch (MPI::Exception failure)
    {
        cerr << "ERRORRRRRRRRRRRR: " << failure.Get_error_string() << endl;
        MPI::COMM_WORLD.Abort(1);
    }
    pthread_mutex_unlock(&recvMutex);
}

void *request_handler(void *arg)
{
    int clock;
    MPI::Status st;
    while (1)
    {
        // printf("%10d %2d:1 is waiting for request...\n", time(NULL), MyId);
        atomicRecv(&clock, 1, MPI::INT, MPI::ANY_SOURCE, REQUEST_TAG, st, 1);
        int source = st.Get_source();
        if (source == MyId)
        {
            // printf("%10d %2d:1 is about to wait...\n", time(NULL), MyId);
            pthread_mutex_lock(&mutex);
            pthread_mutex_unlock(&mutex);
        }
        else
        {
            // printf("%10d %2d:1 handling request from %2d\n", time(NULL), MyId, source);
            LamportClock = max(LamportClock, clock);
            handleStatusRequest(source, 1);
        }
    }
}

void handleStatusRequest(int id, int thread_id)
{
    struct Status s;
    s.clock = LamportClock;
    if (insideState)
    {
        s.state = INSIDE_STATE;
        waiting_for_current_to_exit.push_back(id);
    }
    else if (waitingState)
    {
        s.state = WAITING_STATE;
        s.requesting = requesting;
        if (ProcessStatuses[id].state == WAITING_STATE)
            s.hasMetCurrent = true;
    }
    else
        s.state = OUTSIDE_STATE;
    // printf("%lf %2d:%d before send\n", gettime(NULL), MyId, thread_id);
    atomicSend(&s, sizeof(struct Status), MPI::CHAR, id, STATUS_TAG);
}

int main(int argc, char *argv[])
{
    
    // signal(SIGSEGV, handler);
    // signal(SIGABRT, handler);
    
	int size;

    pthread_t thread;

    int roomSize;
    if (argc < 2)
    {
        cerr << "usage: %s ROOM_SIZE" << endl;
        return 1;
    }
    roomSize = atoi(argv[1]);

    MPI::Init_thread(argc, argv, MPI::THREAD_SERIALIZED);
    size = MPI::COMM_WORLD.Get_size();
    MyId = MPI::COMM_WORLD.Get_rank();

    // switch (provided_thread_support)
    // {
    //     case MPI::THREAD_MULTIPLE:
    //         cout << "multiple";
    //         break;
    //     case MPI::THREAD_SERIALIZED:
    //         cout << "serialized";
    //         break;
    //     case MPI::THREAD_FUNNELED:
    //         cout << "funneled";
    //         break;
    //     case MPI::THREAD_SINGLE:
    //         cout << "single";
    //         break;
    //     default:
    //         cout << "error";
    // }
    // cout << " " << MyId << " of " << size << endl;

    MPI::COMM_WORLD.Set_errhandler(MPI::ERRORS_THROW_EXCEPTIONS);
    
    ProcessStatuses = new struct Status[size]();

    pthread_mutex_init(&mutex, NULL);
    pthread_mutex_init(&recvMutex, NULL);
    // pthread_mutex_init(&recvMutex, NULL);
    pthread_create(&thread, NULL, request_handler, NULL);

    LamportClock = 0;
    
    while (1)
    {
        LamportClock++;

        srand(time(NULL) + MyId);
        int delay = rand() % 10 + 1;
        printf("%5d %2d:0 sleeping for %d seconds before entering the room\n", LamportClock, MyId, delay);
        sleep(delay);
        
        pthread_mutex_lock(&mutex);
        // printf("%lf %2d:0 before send\n", gettime(NULL), MyId);
        atomicSend(&LamportClock, 1, MPI::INT, MyId, REQUEST_TAG); // just dummy send to wake up thread waiting for recv

        waitingState = true;
        LamportClock++;
        requesting = true;
        int numWaiting = 0;
        int numInside = 0;
        for (int i = 0; i < size; i++)
            memset(ProcessStatuses + i, sizeof(struct Status), 0);

        for (int id = 0; id < size; id++)
            if (id != MyId)
                atomicSend(&LamportClock, 1, MPI::INT, id, REQUEST_TAG);

        for (int i = 0; i < size - 1; i++)
        {
            struct Status proc_stat;
            int clock;
            MPI::Status msg_st;
            int tag;
            int source;
            do {
                MPI::COMM_WORLD.Probe(MPI::ANY_SOURCE, MPI::ANY_TAG, msg_st);
                tag = msg_st.Get_tag();
                source = msg_st.Get_source();
                switch (tag)
                {
                    case REQUEST_TAG:
                        // printf("%10d %2d:0 handling request from %d\n",
                        // time(NULL), MyId, source);
                        atomicRecv(&clock, 1, MPI::INT, source, REQUEST_TAG, msg_st, 0);
                        LamportClock = max(LamportClock, clock);
                        handleStatusRequest(source, 0);
                        break;
                    case EXIT_TAG:
                        atomicRecv(&clock, 1, MPI::INT, source, EXIT_TAG, msg_st, 0);
                        LamportClock = max(LamportClock, clock);
                        ProcessStatuses[source].state = OUTSIDE_STATE;
                        break;
                }
                // printf("%10d %2d:0 tut %2d\n", time(NULL), MyId, source);
            } while (tag != STATUS_TAG);
            atomicRecv(&proc_stat, sizeof(struct Status),
                       MPI::CHAR, source, STATUS_TAG, msg_st, 0);
            memcpy(ProcessStatuses + source, &proc_stat, sizeof(struct Status));
            LamportClock = max(LamportClock, ProcessStatuses[source].clock);
            if (ProcessStatuses[source].state == INSIDE_STATE)
                numInside++;
        }

        requesting = false;
        LamportClock++;

        for (int i = 0; i < size; i++)
        {
            struct Status s = ProcessStatuses[i];
            if (i != MyId && s.state == WAITING_STATE)
            {
                if (s.requesting && i > MyId && s.hasMetCurrent)
                    continue;
                numWaiting++;
            }
        }

        // printf("%d %d %d\n", numWaiting, numInside, roomSize);
        
        LamportClock++;

        while (numInside + numWaiting >= roomSize)
        {
            MPI::Status st;
            int clock, tag, source;
            atomicRecv(&clock, 1, MPI::INT, MPI::ANY_SOURCE, MPI::ANY_TAG, st, 0);
            LamportClock = max(LamportClock, clock);
            tag = st.Get_tag();
            source = st.Get_source();
            switch (tag)
            {
                case REQUEST_TAG:
                    handleStatusRequest(source, 0);
                    break;
                case EXIT_TAG:
                    if (clock > ProcessStatuses[source].clock)
                    {
                        if (numWaiting > 0)
                            numWaiting--;
                        else
                            numInside--;
                        break;
                    }
            }
        }
        insideState = true;
        waitingState = false;
        LamportClock++;
        printf("%5d %2d:0 entered the room\n", LamportClock, MyId);
        pthread_mutex_unlock(&mutex);

        delay = rand() % 10 + 1;
        // printf("%10d %d:0 waiting in room for %d seconds\n", time(NULL), MyId, delay);
        sleep(delay);
        pthread_mutex_lock(&mutex);
        atomicSend(&LamportClock, 1, MPI::INT, MyId, REQUEST_TAG);

        insideState = false;
        LamportClock++;
        
        for (vector<int>::iterator it = waiting_for_current_to_exit.begin();
             it != waiting_for_current_to_exit.end(); ++it)
        {
            atomicSend(&LamportClock, 1, MPI::INT, *it, EXIT_TAG);
        }
        waiting_for_current_to_exit.clear();
        
        printf("%5d %2d:0 left the room\n", LamportClock, MyId);

        pthread_mutex_unlock(&mutex);
        
    }

    MPI::Finalize();
}
