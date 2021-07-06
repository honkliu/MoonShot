
/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef EMBEDDINGS_H__
#define EMBEDDINGS_H__

template <typename T>
class Embeddings {
	int m_bytes;
	T* m_data;

	public:
		Embeddings() = default;
		Embeddings(int size)
		{
			m_bytes = sizeof(T) * size;
			m_data = new T[size]

			memset(m_data, 0, m_bytes)
		}
		~Embeddings() 
		{
			if (m_data != nullptr) {
				delete[] m_data;
			}
		}
};
 
#endif

