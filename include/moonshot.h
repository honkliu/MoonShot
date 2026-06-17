/*
* moonshot.h — single include for users of the MoonShot search library.
*
* Link against:  MoonShot::MoonShot   (CMake alias)
* Include:       #include "moonshot.h"
*/

#ifndef MOONSHOT_H__
#define MOONSHOT_H__

/*
* Core pipeline
*/
#include "IndexContext.h"           /* engine — owns PostingStore          */
#include "EvalExpression.h"         /* EvalTree, AndNode, OrNode, TermNode */
#include "IndexSearchCompiler.h"    /* text query  →  EvalTree             */
#include "IndexSearchExecutor.h"    /* EvalTree + ISR  →  SearchResult[]   */

/*
* Index access
*/
#include "AdvancedIndexWriter.h"    /* write tokens into the index          */
#include "AdvancedIndexReader.h"    /* block-backed ISR (on-disk path)      */
#include "IndexReader.h"            /* ISR base interface                   */
#include "IndexReaderImpl.h"        /* TermIndexReader / AndIndexReader / OrIndexReader / NotIndexReader */
#include "PostingStore.h"           /* in-memory posting store              */
#include "SearchResult.h"           /* { doc_id, score, snippet }           */

/*
* Tokenisation
*/
#include "Tokenizer.h"              /* Tokenizer, SmartTokenizer, SimpleTokenizer */

/*
* Vector / embedding search (interface; impl pending)
*/
#include "Embeddings.h"

#endif /* MOONSHOT_H__ */
