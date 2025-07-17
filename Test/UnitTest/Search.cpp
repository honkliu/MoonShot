#include "IndexContext.h"

#include <memory> 

using namespace std;

int main(int argc, char **argv)
{

    auto index_context = new IndexContext("/mnt/nvme_raid0/index", "");

    auto index_writer = index_context->GetWriter();

    auto tokenizer = new SmartTokenizer();

    uint64_t documentId = 32;

    index_writer->Write(tokenizer->Tokenize("The QUICK Brown Fox jumps over the lazy DOG! Привет, МИР! Hello, WORLD! こんにちは这是一个人的世界! I'm testing apostrophes: don't, can't, won't"), documentId, "Body");

    index_writer->Write(tokenizer->Tokenize("Conf 2021"), documentId, "Title");

    auto index_reader = index_context->GetReader("Title");

    index_reader->Open("fox");

    return 0;
}
