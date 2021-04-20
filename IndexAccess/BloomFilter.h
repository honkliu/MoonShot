
/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef BLOOMFILTER_H__
#define BLOOMFILTER_H__

#include <vector> 

using namespace std;

class BloomFilter {
	public:
		BloomFilter() = default;
		void AddElement(unsigned char *elt);
		bool Contains(unsigned char *elt);

	private:
		vector<int> m_HashFunctions;
};
 
#endif

