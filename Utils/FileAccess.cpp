/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/

#include "FileAccess.h"
FileAccess(const char * fileName)
:m_FileName(fileName)
{

}
bool FileAccess::Init()
{
    m_FileHandle = CreateFileA(m_FileName, 
                            )  
    return true;
}
int FileAccess::GetData(void * buffer, int numBytes)
{
    return numBytes;
}


