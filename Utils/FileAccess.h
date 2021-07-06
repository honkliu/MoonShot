
/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef FILEACCESS_H__
#define FILEACCESS_H__

class FileAccess {
	public:
		FileAccess() = default;
		FileAccess(const char * fileName);
		bool Init();
		int GetData(void * buffer, int numBytes);
	private:
		HANDLE m_FileHandle;
		char * m_FileName;	
};
 
#endif

