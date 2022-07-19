# Index Format

There are considerations whether the posting should be stored as a key/value pair using levelDB, Redis, RocksDB and so on.but on the other hand, from current compression requirements, such as Roaring, bitmap, VBC and so on, the postings is still should be organized in a different way for search engine So the index is

* Each [IndexBlock] has 4096 bytes, which is the same as a physical page. 
* Each [IndexFile] is composed of multiple [IndexBlock], it is stored in physical page because it is more efficient. 
* Each [IndexPosting] is a list of documents. A term may have several consecutive IndexPosting across pages.  

IndexBlock
### How index is stored 
### index in disk
### index in memory

# How to acces the index
### Prepare the cache

[IndexBlockTable] is initialized with a number of buffer. it is in the following way. 

A term is hashed to an integer. And there is a list of postings in the table. Look into the IndexBlockTable to access the postings. it means that an iteration is needed to find the table in the memory. 


### Read the index into memory

[IndexBlockTable] is organized in the way, it means that a term could cross Page. A read to a term (posting) generally should load multiple pages at the same time. If we limit the posting length, then the number of pages read a time should be limit to a fixed length, might be 4 at most. 
    
    {Term1, pageStart, pageEnd}
    {Term1, pageStart, pageEnd}
    {Term2, pageStart, pageEnd}
    ....

When a term is looked up in the {Term, pageStart, pageEnd}, Call [IndexBlockTable]._GetIndexBlock(block_seq, number)_ to load the pages into memory if they are not there.

* block_seq is the sequence of the page in the index (memory or ssd), it is the integer, such as: 345, which means that its page number is 345. 
 
### Look up in the index
The order: 
    Term
    -->[ElementFilter] (in Memory, Judge whether the index in existing in the index) 
    -->[TermToBlock](in Memory, find out the physical number of the block in the index)
    -->[IndexBlockTable] (In memory)
        -->If [IndexBlockTable] Contains the [IndexBlock], return the Seriel Number
        -->If [IndexBlockTable] does not contain the [IndexBlock], Do IO, Read the [IndexBlock] from [IndexFile]->[IF_Data]
    -->[BlockHeader] Decoding from the [IndexBlock]
    -->[SkipList] decoding from the [IndexBlock]
    -->Get the [Posting] for further use. 


# Received a query
### 1. Tokenize [SmartTokenizer] or Vectorize 
### 2. Compile into [EvalTree] or [Embeddings]
* Into an [EvalTree], [IndexSearchCompiler].Compile(string)
* Into an [Embeddings], [IndexSearchCompiler].CompileToVector(string)
### 3. Use [IndexContext] to get [AdvancedIndexReader] with [EvalTree] or [Embeddings]
```c 
    auto reader = indexContext.GetReader(EvalTree * eval_tree);
```
### 4. Iterate document candidate with _GoNext()_ of [AdvancedIndexReader]  
---

# How to match a document

After a query is passed to a reader, the reader will stop at the first match, so the match could be:
* The first _MATCHED_ document if we follow the _AND_ syntax
* The first document if we follow the _OR_ Syntax  
### 1. Compose the [AdvancedIndexReader] object
An [AdvancedIndexReader] is composed within the [IndexContext], it is passed with an [EvalTree] or [Embeddings], so we could list the postings from inverted index or vectors. After the object get initialized, it will be connected to the physical entity. 

### 2. Assign the postings to  [AdvancedIndexReader]
In order to assign the postings to the reader, it need to. 
* Check [ElementFilter] to conclude whether the posting exists in the index. 
* If the index exist, Look up the [TermToBlock] to find the corresponding [IndexBlock] Num.



* Load the [Page] into [IndexBlockTable] for further use. 
* Return a reference to [Page], so [AdvancedIndexReader] could iterate.   

Adding more
### 3. 

[SmartTokenizer]: Tokenizer/SmartTokenizer.cpp
[IndexSearchCompiler]: Compiler/IndexSearchCompiler.h
[EvalTree]:Compiler/EvalExpression.h
[Embeddings]:Compiler/EvalExpression.h
[IndexSearchExecutor]: Executor/IndexSearchExecutor.h
[AdvancedIndexReader]: IndexAccess/AdvancedIndexReader.h
[ElementFilter]: IndexAccess/ElementFilter.h
[IndexContext]: IndexAccess/IndexContext.h
[IndexBlockTable]:IndexAccess/BlockTable.h


