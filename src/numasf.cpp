#include "numasf.h"
#include "bitboard.h"

#include <map>
#include <set>
#include <string>
#include <algorithm>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <numa.h>
#endif

NumaState NumaInfo; // Global object

NumaState::NumaState() {

  coreCount = 0;
  nodeVector.clear();

#ifdef _WIN32   // use windows

  DWORD returnLength = 0;
  DWORD byteOffset = 0;
  imp_GetLogicalProcessorInformationEx = (GLPIEX)GetProcAddress(GetModuleHandle("kernel32.dll"),"GetLogicalProcessorInformationEx");
  imp_SetThreadGroupAffinity = (STGA)GetProcAddress(GetModuleHandle("kernel32.dll"),"SetThreadGroupAffinity");

  if (imp_GetLogicalProcessorInformationEx != nullptr
             && imp_SetThreadGroupAffinity != nullptr) {
    // use windows processor groups

    // get array of node and core data
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* buffer = nullptr;
    while (true) {
      if (imp_GetLogicalProcessorInformationEx(RelationAll, buffer, &returnLength))
        break;
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        free(buffer);
        buffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)malloc(returnLength);
        if (!buffer) {
          std::cerr << "GetLogicalProcessorInformationEx malloc failed" << std::endl;
          exit(EXIT_FAILURE);
        }
      } else {
        free(buffer);
        std::cerr << "GetLogicalProcessorInformationEx failed" << std::endl;
        exit(EXIT_FAILURE);
      }
    }

    // first get nodes
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* ptr = buffer;
    while ((ptr->Size > 0) && (byteOffset + ptr->Size <= returnLength)) {
      if (ptr->Relationship == RelationNumaNode) {
        nodeVector.push_back(NumaNode(ptr->NumaNode.NodeNumber, ptr->NumaNode.GroupMask));
      }
    byteOffset += ptr->Size;
    ptr = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(((char*)ptr) + ptr->Size);        
    }

    // then count cores in each node
    ptr = buffer; byteOffset = 0;
    while ((ptr->Size > 0) && (byteOffset + ptr->Size <= returnLength)) {
      if (ptr->Relationship == RelationProcessorCore) {
        // loop through nodes to find one with matching group number and intersecting mask
        for (std::vector<NumaNode>::iterator node = nodeVector.begin(); node != nodeVector.end(); ++node) {
          if (node->groupMask.Group == ptr->Processor.GroupMask[0].Group
              && ((node->groupMask.Mask & ptr->Processor.GroupMask[0].Mask) != 0)) {
            node->coreCount++;
            coreCount++;
            break;
          }            
        }
      }
    byteOffset += ptr->Size;
    ptr = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(((char*)ptr) + ptr->Size);        
    }
    free(buffer);

  } else {
    // use windows but not its processor groups

    // get array of node and core data
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buffer = nullptr;
    while (true) {
      if (GetLogicalProcessorInformation(buffer, &returnLength))
        break;
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        free(buffer);
        buffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)malloc(returnLength);
        if (!buffer) {
          std::cerr << "GetLogicalProcessorInformation malloc failed" << std::endl;
          exit(EXIT_FAILURE);
        }
      } else {
        free(buffer);
        std::cerr << "GetLogicalProcessorInformation failed" << std::endl;
        exit(EXIT_FAILURE);
      }
    }

    // first get nodes
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* ptr = buffer;
    while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) {
      if (ptr->Relationship == RelationNumaNode) {
        nodeVector.push_back(NumaNode(ptr->NumaNode.NodeNumber, ptr->ProcessorMask));
      }
      byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
      ptr++;
    }

    // then count cores in each node
    ptr = buffer; byteOffset = 0;
    while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) {
      if (ptr->Relationship == RelationProcessorCore) {
        // loop through nodes to find one with intersecting mask
        for (std::vector<NumaNode>::iterator node = nodeVector.begin(); node != nodeVector.end(); ++node) {
          if ((node->mask & ptr->ProcessorMask) != 0) {
            node->coreCount++;
            coreCount++;
            break;
          }            
        }
      }
      byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
      ptr++;
    }
    free(buffer);
  }

    // create a dummy node if something went wrong
    // upon return, nodeVector is not supposed to be empty
    if (coreCount == 0) {
        nodeVector.clear();
        coreCount = 1;
    }

    if (nodeVector.size() == 0) {
        DWORD_PTR procMask;
        DWORD_PTR sysMask;
        GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
        nodeVector.push_back(NumaNode(-1, procMask));
    }


#else // use linux

  if (numa_available() != -1) {

      // first get nodes
      // library function numa_node_to_cpus parses for us the files
      //  /sys/devices/system/node/nodeN/cpumap
      for (int N = 0; N <= numa_max_node(); N++) {
//sync_cout << "looking at node" << N << sync_endl;        
        bitmask* cpuBitset = numa_allocate_cpumask();
        if (numa_node_to_cpus(N, cpuBitset) != 0) {
          // something went wrong. just free the bitmask in this case
          numa_bitmask_free(cpuBitset);
        } else {
//sync_cout << "node" << N << " added!" << sync_endl;        
          // otherwise pass it to a node
          nodeVector.push_back(NumaNode(N, cpuBitset));
        }
      }

      // then count cores in each node
      //   suppose that cpu0 and cpu4 share the same core
      //   then both /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
      //         and /sys/devices/system/cpu/cpu4/topology/thread_siblings_list
      //   contain "0,4\n"     
      std::string baseDir("/sys/devices/system/cpu");
      for (int i = 0; ; i++) {
//sync_cout << "looking at cpu" << i << sync_endl;        
          std::string cpuDir(baseDir + "/cpu" + std::to_string(i));
          if (i > 0) {
              std::ifstream is(cpuDir + "/online");
              if (!is)
                  break;
              std::string line;
              std::getline(is, line);
              if (!is || is.eof() || (line != "1"))
                  continue; 
          }
//sync_cout << "cpu" << i << " is online"<< sync_endl;        
          std::ifstream is(cpuDir + "/topology/thread_siblings_list");
          if (is) {
              std::string line;
              std::getline(is, line);
              if (is && !is.eof()) {
//sync_cout << "looking at cpu" << i << " thread_siblings_list"<< sync_endl;
                  // get the first integer on the line
                  // FIXME: is this call to find_first_of necessary? does stoi stop parsing at '\n' or ',' or '-' ? 
                  auto pos = line.find_first_of(",-");
                  if (pos != std::string::npos)
                      line = line.substr(0, pos);
                  if (std::stoi(line) == i) {
//sync_cout << "cpu" << i << " represets a core!"<< sync_endl;
                      // at this point i represents a core
                      // In the example above, i=0 reaches here but i=4 does not.
                      // loop through nodes to find one with intersecting mask
                      for (std::vector<NumaNode>::iterator node = nodeVector.begin(); node != nodeVector.end(); ++node) {
                          if (numa_bitmask_isbitset(node->cpuBitset, i)) {
//sync_cout << "cpu" << i << " added to core count of node" << node->nodeNumber << "!"<< sync_endl;
                            node->coreCount++;
                            coreCount++;
                            break;
                          }
                      }                       
                  }
              }
          }
      }
  }

  // create a dummy node if something went wrong
  // upon return, nodeVector is not supposed to be empty
  if (coreCount == 0) {
    nodeVector.clear();
    coreCount = 1;
  }

  if (nodeVector.size() == 0) {
    // give the dummy node a full mask
    bitmask* cpuBitset= numa_allocate_cpumask();
    numa_bitmask_setall(cpuBitset);
    nodeVector.push_back(NumaNode(-1, cpuBitset));
  }

#endif

}


NumaNode* NumaState::nodeForThread(size_t threadIdx) {

    if (nodeVector.size() == 1)
        return &nodeVector[0];
    
    // This is just one of many ways to assign idxs to nodes
    // It might be a good idea take into consideration what node the OS has started us on
    int i = threadIdx % coreCount;
    for (size_t n = 0; n < nodeVector.size(); n++) {
        i -= nodeVector[n].coreCount;
        if (i < 0)
            return &nodeVector[n];
    }

    // it is not expected to get here
    return &nodeVector[0];
}


void NumaState::bindThread(NumaNode* numaNode) {

  // don't bind dummy node
  if (numaNode->nodeNumber == -1)
      return;

#ifdef _WIN32   // use windows

  if (numaNode->groupMask.Group != 0xFFFF) {
      // use windows processor groups
      if (!imp_SetThreadGroupAffinity(GetCurrentThread(), &numaNode->groupMask, NULL)) {
        sync_cout << "\nSetThreadGroupAffinity failed" << sync_endl;
      }
  } else {
      // use windows but not its processor groups
      if (SetThreadAffinityMask(GetCurrentThread(), numaNode->mask) == 0) {
        sync_cout << "\nSetThreadAffinityMask failed" << sync_endl;
      }
  }

#else // use linux

  numa_sched_setaffinity(0, numaNode->cpuBitset);
  numa_set_preferred(numaNode->nodeNumber);

#endif
}