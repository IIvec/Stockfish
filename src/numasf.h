#ifndef NUMASF_H_
#define NUMASF_H_

#include <vector>
#include <iostream>
#include <sstream>

#include "misc.h"
#include "movepick.h"

#ifdef _WIN32
#include <windows.h>
typedef BOOL (WINAPI *GLPIEX)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);
typedef BOOL (WINAPI *STGA)(HANDLE, GROUP_AFFINITY*, PGROUP_AFFINITY);
#else
#include <numa.h>
#endif



class NumaNode {

public:

  // pointer to our perthread cmh
  CounterMoveHistoryStats* cmhTable;

  // the node number assigned by OS
  // this is a DWORD on windows
  // DWORD the same as int?
  int nodeNumber;

  // the number of physical cores in this node
  // this will be <= the number of logical processors in this node
  size_t coreCount;

  // now keep a bitset of logical processors in this node
  // this is OS dependent 
#ifdef _WIN32

  // this is used when using windows processor groups
  GROUP_AFFINITY groupMask;   // GROUP_AFFINITY structure contains a group number and a 64bit mask

  // this is used when using windows but not its processor groups
  // the groupMask.Group should be -1 in this case
  // it might be possible to use groupMask.Mask member in this case
  ULONGLONG mask;             // just keep a 64bit mask

#else   // don't use windows

  // This is a bitset of variable size
  // cpuBitset is a pointer to an allocated bitmask struct
  //    which itself contains a pointer to a bitmask of variable size
  // provided by libnuma
  bitmask* cpuBitset;

#endif



  // various OS dependent node constructors
  // at construction per node memory pointers are null
  // and allocated once needed
#ifdef _WIN32
  // use windows processor groups
  NumaNode(DWORD _nodeNumber, GROUP_AFFINITY _groupMask) {
      nodeNumber = _nodeNumber;
      coreCount = 0;
      cmhTable = nullptr;
      groupMask= _groupMask;
      mask = 0;
  }
  // use windows but not its processor groups
  NumaNode(DWORD _nodeNumber, ULONGLONG _mask) {
      nodeNumber = _nodeNumber;
      coreCount = 0;
      cmhTable = nullptr;
      groupMask.Group = 0xFFFF;
      groupMask.Mask = 0;
      mask = _mask;
  }

#else   // use linux
  NumaNode(int _nodeNumber, bitmask* _cpuBitset) {
      nodeNumber = _nodeNumber;
      coreCount = 0;
      cmhTable = nullptr;
      cpuBitset = _cpuBitset;
  }
#endif


  // when copying a node we do not want to copy cmhTable
  NumaNode(const NumaNode &source) {
    nodeNumber = source.nodeNumber;
    coreCount = source.coreCount;
    cmhTable = nullptr;
#ifdef _WIN32
    groupMask = source.groupMask;
    mask = source.mask;
#else   // use linux
    cpuBitset = numa_allocate_cpumask();
    copy_bitmask_to_bitmask(source.cpuBitset, cpuBitset);
#endif
  }


  // destructing a node involved freeing the per node memory
  // and the variable size cpuBitset on linux
  ~NumaNode() {
    if (cmhTable != nullptr) {
      delete cmhTable;
    }
    cmhTable = nullptr;
#ifndef _WIN32  // use linux
    numa_bitmask_free(cpuBitset);
#endif
  }


  // various OS dependent printers
  std::string print() {
      std::ostringstream ss;      
      std::stringstream ss2;

      ss2 << std::hex << cmhTable;
      std::string cmh_str(ss2.str());
      cmh_str.erase(0, (std::min)(cmh_str.find_first_not_of('0'), cmh_str.size() - 1));
      cmh_str.erase(0, (std::min)(cmh_str.find_first_not_of('x'), cmh_str.size() - 1));

#ifdef _WIN32
      if (groupMask.Group != 0xFFFF)      
      {
          // use windows processor groups
          std::stringstream ss3;
          ss3 << std::hex << (void*)groupMask.Mask;
          std::string gmask_str(ss3.str());
          gmask_str.erase(0, (std::min)(gmask_str.find_first_not_of('0'), gmask_str.size() - 1));
          gmask_str.erase(0, (std::min)(gmask_str.find_first_not_of('x'), gmask_str.size() - 1));
          ss << "nodeNr.: " << nodeNumber << ", cores: " << coreCount << ", cmh: " << cmh_str << ", Group: " << groupMask.Group << ", Mask: " << gmask_str << "\n";
      } 
      else
      {
          // use windows but not its processor groups
          std::stringstream ss3;
          ss3 << std::hex << (void*)mask;
          std::string mask_str(ss3.str());
          mask_str.erase(0, (std::min)(mask_str.find_first_not_of('0'), mask_str.size() - 1));
          mask_str.erase(0, (std::min)(mask_str.find_first_not_of('x'), mask_str.size() - 1));
          ss << "nodeNr.: " << nodeNumber << ", cores: " << coreCount << ", cmh: " << cmh_str << ", mask: " << mask_str << "\n";
      }  
#else   // use linux
      ss << "cpuBitset: ";
      for (unsigned int i = 0; i < 8*numa_bitmask_nbytes(cpuBitset); i++) {
          if (numa_bitmask_isbitset(cpuBitset, i))
              ss << " " << i;
      }

#endif
      return ss.str();
  }

};



class NumaState {
public:

  // imported functions that are not present in all kernel32.dll's
#ifdef _WIN32
  GLPIEX imp_GetLogicalProcessorInformationEx;
  STGA   imp_SetThreadGroupAffinity;
#endif

  // if numa functions are not present,
  //   there should be a dummy node in it with nodeNumber = -1
  // this vector should not be empty
  std::vector<NumaNode> nodeVector;
    
  // total number of cores in all nodes
  // this should not be 0
  // it is set to 1 when using a dummy node
  size_t coreCount;

  // preferred node for a given search thread
  NumaNode* nodeForThread(size_t threadNo);

  // bind current thread to node
  void bindThread(NumaNode* numaNode);

  // per node memory allocation
  // this is done with the help of NumaState struct because it may later use imported functions
  //   for the allocation instead of new operator
  // for example the new opterator rarely returns an address that is page-aligned
  // also there is nogaurantee where the memory is allocated with new
  CounterMoveHistoryStats* getCmhTable(NumaNode* node){
      if (node->cmhTable == nullptr) {
          node->cmhTable = new CounterMoveHistoryStats; 
      }
      return node->cmhTable;
  }

  NumaState();

  // print out all of the nodes to stdout
  void display() {
      sync_cout << "\nNuma Hardware Configuration:" << sync_endl;
      for (std::vector<NumaNode>::iterator node = nodeVector.begin(); node != nodeVector.end(); ++node) {
          sync_cout << node->print() << sync_endl;         
      }
  }



};


extern NumaState NumaInfo;

#endif