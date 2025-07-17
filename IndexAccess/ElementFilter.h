
/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef ELEMENTFILTER_H__
#define ELEMENTFILTER_H__


/*
*	Here you could use different filters such as BloomFilter, CuckookFilter, XorFilter 
*	and so on. From the description, xor filter outperform others. 
*/
class ElementFilter {
	public:
		ElementFilter(int size = 24, void * memory = NULL);
		~ElementFilter();
		void AddElement(const char *elt);
		bool Contains(const char *elt);
	private:
		int m_size; 
		unsigned char * m_FilterSpace;	
};
 
#endif

