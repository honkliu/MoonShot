# Index Format
### How index is stored 
### index in disk
### index in memory

# How to acces the index
### Prepare the cache
### Read the index into memory
 

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
* If the index exist, Look up the [TermToTable] to find the corresponding [page].
* Load the [Page] into [BlockTable] for further use. 
* Return a reference to [Page], so [AdvancedIndexReader] could iterate.   


### 3. 

[SmartTokenizer]: Tokenizer/SmartTokenizer.cpp
[IndexSearchCompiler]: Compiler/IndexSearchCompiler.h
[EvalTree]:Compiler/EvalExpression.h
[Embeddings]:Compiler/EvalExpression.h
[IndexSearchExecutor]: Executor/IndexSearchExecutor.h
[AdvancedIndexReader]: IndexAccess/AdvancedIndexReader.h
[ElementFilter]: IndexAccess/ElementFilter.h
[IndexContext]: IndexAccess/IndexContext.h



