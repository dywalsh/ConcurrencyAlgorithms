#include "stdafx.h"                             // pre-compiled headers
#include <iostream>                             // cout
#include <iomanip>                              // setprecision
#include "helper.h"                             //

using namespace std;                            // cout

#define K           1024                        //
#define GB          (K*K*K)                     //
#define NOPS        10000                       //
#define NSECONDS    2                           // run each test for NSECONDS

#define COUNTER64                               // comment for 32 bit counter
												//#define FALSESHARING                          // allocate counters in same cache line
												//#define USEPMS                                // use PMS counters

#ifdef COUNTER64
#define VINT    UINT64                          //  64 bit counter
#else
#define VINT    UINT                            //  32 bit counter
#endif

#define ALIGNED_MALLOC(sz, align) _aligned_malloc(sz, align)

#ifdef FALSESHARING
#define GINDX(n)    (g+n)                       //
#else
#define GINDX(n)    (g+n*lineSz/sizeof(VINT))   //
#endif


UINT64 tstart;                                  // start of test in ms
int sharing;                                    // % sharing
int lineSz;                                     // cache line size
int maxThread;                                  // max # of threads

THREADH *threadH;                               // thread handles
UINT64 *ops;                                    // for ops per thread

#if OPTYP == 3
UINT64 *aborts;                                 // for counting aborts
#endif

typedef struct {
	int sharing;                                // sharing
	int nt;                                     // # threads
	UINT64 rt;                                  // run time (ms)
	UINT64 ops;                                 // ops
	UINT64 incs;                                // should be equal ops
	UINT64 aborts;                              //
} Result;

Result *r;                                      // results
UINT indx;                                      // results index

volatile VINT *g;                               // NB: position of volatile

												//
												// test memory allocation [see lecture notes]
												//
ALIGN(64) UINT64 cnt0;
ALIGN(64) UINT64 cnt1;
ALIGN(64) UINT64 cnt2;
UINT64 cnt3;                                    // NB: in Debug mode allocated in cache line occupied by cnt0

//Test And Test And Set vars
volatile long long tataslock = 0;
//Bakery vars
int MAXTHREAD = 16;
int number[16]; 
int choosing[16];
int p_id;
//MCS vars


// MCS 
class ALIGNEDMA {
public:
	void* operator new(size_t); // override new
	void operator delete(void*); // override delete
};

//
// new
//
void* ALIGNEDMA::operator new(size_t sz) { // aligned memory allocator
	sz = (sz + lineSz - 1) / lineSz * lineSz; // make sz a multiple of lineSz
	return _aligned_malloc(sz, lineSz); // allocate on a lineSz boundary
}
//
// delete
//
void ALIGNEDMA::operator delete(void *p) {
	_aligned_free(p); // free object
}

class Object : public ALIGNEDMA {
public:
	volatile int x;
	volatile int y;
};

class QNode : public ALIGNEDMA {
public:
	volatile int waiting;
	volatile QNode *next;
};

//MCS VARS
QNode * lock = NULL;

DWORD tlsIndex = TlsAlloc();;


//
// OPTYP
//
// 0:inc
// 1:InterlockedIncrement
// 2:InterlockedCompareExchange
// 3:RTM (restricted transactional memory)
// 4:TestAndTestAndSet Lock
// 5:Bakery Lock
// 6:MCS Lock
//

#define OPTYP       5                      // set op type

#if OPTYP == 0

#define OPSTR       "inc"
#define INC(g)      (*g)++;

#elif OPTYP == 1

#ifdef COUNTER64
#define OPSTR       "InterlockedIncrement64"
#define INC(g)      InterlockedIncrement64((volatile LONG64*) g)
#else
#define OPSTR       "InterlockedIncrement"
#define INC(g)      InterlockedIncrement(g)
#endif

#elif OPTYP == 2

#ifdef COUNTER64
#define OPSTR       "InterlockedCompareExchange64"
#define INC(g)      do {                                                                        \
                        x = *g;                                                                 \
                    } while (InterlockedCompareExchange64((volatile LONG64*) g, x+1, x) != x);
#else
#define OPSTR       "InterlockedCompareExchange"
#define INC(g)      do {                                                                        \
                        x = *g;                                                                 \
                    } while (InterlockedCompareExchange(g, x+1, x) != x);
#endif

#elif OPTYP == 3

#define OPSTR       "RTM (restricted transactional memory)"
#define INC(g)      {                                                                           \
                        UINT status = _xbegin();                                                \
                        if (status == _XBEGIN_STARTED) {                                        \
                            (*g)++;                                                             \
                            _xend();                                                            \
                        } else {                                                                \
                            nabort++;                                                           \
                            InterlockedIncrement64((volatile LONG64*)g);                        \
                        }                                                                       \
                    }
// Test And Test And Set Lock
#elif OPTYP == 4
#define OPSTR	"TestAndTestAndSet"
#define INC(g)		while(InterlockedExchange64(&tataslock,1)) \
						while(tataslock == 1)				  \
								_mm_pause();			  \
					(*g)++;								  \
                    tataslock = 0;
// BAKERY Lock
#elif OPTYP == 5
#define OPSTR	"Bakery Lock"
#define INC(g)	acquire(p_id);	\
				(*g)++;					\
				release(p_id);	\

void acquire(int pid) { // pid is thread ID
	choosing[pid] = 1;
	int max = 0;
	_mm_mfence();
	for (int i = 0; i < maxThread; i++) { // find maximum ticket
		if (number[i] > max)
			max = number[i];
	}
	number[pid] = max + 1; // our ticket number is maximum ticket found + 1
	choosing[pid] = 0;
	_mm_mfence();
	for (int j = 0; j < maxThread; j++) { // wait until our turn i.e. have lowest ticket
		while (choosing[j]); // wait while thread j choosing
		while (number[j] && ((number[j] < number[pid]) || ((number[j] == number[pid]) && (j < pid))));
	}
}

void release(int pid) {
	number[pid] = 0; // release lock
}

//MCS Lock
#elif OPTYP == 6
#define OPSTR	"MCS"
#define INC(g)      acquire(&lock);     \
                    (*g)++;				\
                    release(&lock);   

void acquire(QNode **lock) {
	volatile QNode *qn = (QNode*)TlsGetValue(tlsIndex);
	qn->next = NULL;
	volatile QNode *pred = (QNode*)InterlockedExchangePointer((PVOID*)lock, (PVOID)qn);
	if (pred == NULL)
		return; // have lock
	qn->waiting = 1;
	pred->next = qn;
	while (qn->waiting) Sleep(0);
}
void release(QNode **lock) {
	volatile QNode *qn = (QNode*)TlsGetValue(tlsIndex);
	volatile QNode *succ;
	if (!(succ = qn->next)) {
		if (InterlockedCompareExchangePointer((PVOID*)lock, NULL, (PVOID)qn) == qn)
			return;
		while ((succ = qn->next) == NULL); // changed from do … while()
	}
	succ->waiting = 0;
}
#endif

//
// worker
//
WORKER worker(void *vthread)
{
	int thread = (int)((size_t)vthread);

	UINT64 n = 0;

	volatile VINT *gt = GINDX(thread);
	volatile VINT *gs = GINDX(maxThread);

#if OPTYP == 2
	VINT x;
#elif OPTYP == 3
	UINT64 nabort = 0;
#elif OPTYP == 6
	QNode *qn = new QNode();
	qn->next = NULL;
	qn->waiting = 0;
	TlsSetValue(tlsIndex, qn);
#endif

	while (1) {

		//
		// do some work
		//
		for (int i = 0; i < NOPS / 4; i++) {

			switch (sharing) {
			case 0:

				INC(gt);
				INC(gt);
				INC(gt);
				INC(gt);
				break;

			case 25:

				INC(gt);
				INC(gt);
				INC(gt);
				INC(gs);
				break;

			case 50:
				INC(gt);
				INC(gs);
				INC(gt);
				INC(gs);
				break;

			case 75:
				INC(gt);
				INC(gs);
				INC(gs);
				INC(gs);
				break;

			case 100:
				INC(gs);
				INC(gs);
				INC(gs);
				INC(gs);

			}
		}
		n += NOPS;

		//
		// check if runtime exceeded
		//
		if ((getWallClockMS() - tstart) > NSECONDS * 1000)
			break;
	}

	ops[thread] = n;
#if OPTYP == 3
	aborts[thread] = nabort;
#endif

	return 0;

}

//
// main
//
int main()
{
	ncpu = getNumberOfCPUs();   // number of logical CPUs
	maxThread = 2 * ncpu;       // max number of threads

								//
								// get date
								//
	char dateAndTime[256];
	getDateAndTime(dateAndTime, sizeof(dateAndTime));

	//
	// console output
	//
	cout << getHostName() << " " << getOSName() << " sharing " << (is64bitExe() ? "(64" : "(32") << "bit EXE)";
#ifdef _DEBUG
	cout << " DEBUG";
#else
	cout << " RELEASE";
#endif
	cout << " [" << OPSTR << "]" << " NCPUS=" << ncpu << " RAM=" << (getPhysicalMemSz() + GB - 1) / GB << "GB " << dateAndTime << endl;
#ifdef COUNTER64
	cout << "COUNTER64";
#else
	cout << "COUNTER32";
#endif
#ifdef FALSESHARING
	cout << " FALSESHARING";
#endif
	cout << " NOPS=" << NOPS << " NSECONDS=" << NSECONDS << " OPTYP=" << OPTYP;
#ifdef USEPMS
	cout << " USEPMS";
#endif
	cout << endl;
	cout << "Intel" << (cpu64bit() ? "64" : "32") << " family " << cpuFamily() << " model " << cpuModel() << " stepping " << cpuStepping() << " " << cpuBrandString() << endl;
#ifdef USEPMS
	cout << "performance monitoring version " << pmversion() << ", " << nfixedCtr() << " x " << fixedCtrW() << "bit fixed counters, " << npmc() << " x " << pmcW() << "bit performance counters" << endl;
#endif

	//
	// get cache info
	//
	lineSz = getCacheLineSz();
	//lineSz *= 2;

	if ((&cnt3 >= &cnt0) && (&cnt3 < (&cnt0 + lineSz / sizeof(UINT64))))
		cout << "Warning: cnt3 shares cache line used by cnt0" << endl;
	if ((&cnt3 >= &cnt1) && (&cnt3 < (&cnt1 + lineSz / sizeof(UINT64))))
		cout << "Warning: cnt3 shares cache line used by cnt1" << endl;
	if ((&cnt3 >= &cnt2) && (&cnt3 < (&cnt2 + lineSz / sizeof(UINT64))))
		cout << "Warning: cnt2 shares cache line used by cnt1" << endl;

#if OPTYP == 3

	//
	// check if RTM supported
	//
	if (!rtmSupported()) {
		cout << "RTM (restricted transactional memory) NOT supported by this CPU" << endl;
		quit();
		return 1;
	}

#endif

	cout << endl;

	//
	// allocate global variable
	//
	// NB: each element in g is stored in a different cache line to stop false sharing
	//
	threadH = (THREADH*)ALIGNED_MALLOC(maxThread * sizeof(THREADH), lineSz);             // thread handles
	ops = (UINT64*)ALIGNED_MALLOC(maxThread * sizeof(UINT64), lineSz);                   // for ops per thread

#if OPTYP == 3
	aborts = (UINT64*)ALIGNED_MALLOC(maxThread * sizeof(UINT64), lineSz);                // for counting aborts
#endif

#ifdef FALSESHARING
	g = (VINT*)ALIGNED_MALLOC((maxThread + 1) * sizeof(VINT), lineSz);                     // local and shared global variables
#else
	g = (VINT*)ALIGNED_MALLOC((maxThread + 1)*lineSz, lineSz);                         // local and shared global variables
#endif

#ifdef USEPMS

	fixedCtr0 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu * sizeof(UINT64), lineSz);      // for fixed counter 0 results
	fixedCtr1 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu * sizeof(UINT64), lineSz);      // for fixed counter 1 results
	fixedCtr2 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu * sizeof(UINT64), lineSz);      // for fixed counter 2 results
	pmc0 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu * sizeof(UINT64), lineSz);           // for performance counter 0 results
	pmc1 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu * sizeof(UINT64), lineSz);           // for performance counter 1 results
	pmc2 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu * sizeof(UINT64), lineSz);           // for performance counter 2 results
	pmc3 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu * sizeof(UINT64), lineSz);           // for performance counter 3 results

#endif

	r = (Result*)ALIGNED_MALLOC(5 * maxThread * sizeof(Result), lineSz);                   // for results
	memset(r, 0, 5 * maxThread * sizeof(Result));                                           // zero

	indx = 0;



	//
	// use thousands comma separator
	//
	setCommaLocale();

	//
	// header
	//
	cout << "sharing";
	cout << setw(4) << "nt";
	cout << setw(6) << "rt";
	cout << setw(16) << "ops";
	cout << setw(6) << "rel";
#if OPTYP == 3
	cout << setw(8) << "commit";
#endif
	cout << endl;

	cout << "-------";              // sharing
	cout << setw(4) << "--";        // nt
	cout << setw(6) << "--";        // rt
	cout << setw(16) << "---";      // ops
	cout << setw(6) << "---";       // rel
#if OPTYP == 3
	cout << setw(8) << "------";
#endif
	cout << endl;

	//
	// boost process priority
	// boost current thread priority to make sure all threads created before they start to run
	//
#ifdef WIN32
	//  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	//  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif

	//
	// run tests
	//
	UINT64 ops1 = 1;

	for (sharing = 0; sharing <= 100; sharing += 25) {

		for (int nt = 1; nt <= maxThread; nt *= 2, indx++) {

			//
			//  zero shared memory
			//
			for (int thread = 0; thread < nt; thread++)
				*(GINDX(thread)) = 0;   // thread local
			*(GINDX(maxThread)) = 0;    // shared


										//
										// get start time
										//
			tstart = getWallClockMS();

			//
			// create worker threads
			//
			for (int thread = 0; thread < nt; thread++)
				createThread(&threadH[thread], worker, (void*)(size_t)thread);

			//
			// wait for ALL worker threads to finish
			//
			waitForThreadsToFinish(nt, threadH);
			UINT64 rt = getWallClockMS() - tstart;



			//
			// save results and output summary to console
			//
			for (int thread = 0; thread < nt; thread++) {
				r[indx].ops += ops[thread];
				r[indx].incs += *(GINDX(thread));
#if OPTYP == 3
				r[indx].aborts += aborts[thread];
#endif
			}
			r[indx].incs += *(GINDX(maxThread));
			if ((sharing == 0) && (nt == 1))
				ops1 = r[indx].ops;
			r[indx].sharing = sharing;
			r[indx].nt = nt;
			r[indx].rt = rt;

			cout << setw(6) << sharing << "%";
			cout << setw(4) << nt;
			cout << setw(6) << fixed << setprecision(2) << (double)rt / 1000;
			cout << setw(16) << r[indx].ops;
			cout << setw(6) << fixed << setprecision(2) << (double)r[indx].ops / ops1;

#if OPTYP == 3

			cout << setw(7) << fixed << setprecision(0) << 100.0 * (r[indx].ops - r[indx].aborts) / r[indx].ops << "%";

#endif

			if (r[indx].ops != r[indx].incs)
				cout << " ERROR incs " << setw(3) << fixed << setprecision(0) << 100.0 * r[indx].incs / r[indx].ops << "% effective";

			cout << endl;

			//
			// delete thread handles
			//
			for (int thread = 0; thread < nt; thread++)
				closeThread(threadH[thread]);

		}

	}

	cout << endl;

	//
	// output results so they can easily be pasted into a spread sheet from console window
	//
	setLocale();
	cout << "sharing/nt/rt/ops psec/incs psec";
#if OPTYP == 3
	cout << "/aborts";
#endif
	cout << endl;
	for (UINT i = 0; i < indx; i++) {
		//check if incs == ops
		char equals[] = "Equals";
		char NotEquals[] = "Not Equals";
		char *cmpResults;
		if (r[i].ops == r[i].incs) cmpResults = equals;
		else cmpResults = NotEquals;
		cout << r[i].sharing << "/" << r[i].nt << "/" << r[i].rt << "/" << (r[i].ops) << "/" 
			<< r[i].incs << "/" << cmpResults;
#if OPTYP == 3
		cout << "/" << r[i].aborts;
#endif
		cout << endl;
	}
	cout << endl;



	quit();

	return 0;

}

// eof
