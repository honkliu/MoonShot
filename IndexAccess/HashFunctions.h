
/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/
#ifndef HASHFUNCTIONS_H__
#define HASHFUNCTIONS_H__

/*
* Following https://www.eecs.harvard.edu/~michaelm/postscripts/tr-02-05.pdf
* we only need to compute the hash function twice for a bloomfilter.
*/
int Hash1(unsigned char *, int size);
int Hash2(unsigned char *, int size);
int HashMurmur3(unsigned char *, int size, int seed = 3);
int HashCrypto(unsigned char *, int size);

 
#endif

