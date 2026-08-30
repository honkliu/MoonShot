//! Direct translation of the C++ reader implementations; symbol names stay aligned for debugging.
#![allow(non_snake_case, non_upper_case_globals)]

use crate::block_table::DocDataEntry;
use crate::embeddings::VectorSearchResult;
use crate::index_reader::{IndexReader, NO_MORE_DOCS, READER_SOURCE_VECTOR};

/* ------------------------------------------------------------------ */

/*
* AndIndexReader — DAAT intersection: all children must land on the same doc.
* AlignToPivot drives all children to the maximum current doc ID,
* restarting until all agree or any child exhausts.
*/
#[allow(non_snake_case)]
pub struct AndIndexReader {
    m_Children: Vec<Box<dyn IndexReader>>,
    m_Debug: bool,
    m_DebugDepth: usize,
}

#[allow(non_snake_case)]
impl AndIndexReader {
    pub fn new(children: Vec<Box<dyn IndexReader>>) -> Self {
        let mut r = Self {
            m_Children: children,
            m_Debug: false,
            m_DebugDepth: 0,
        };
        r.AlignToPivot();
        r
    }

    fn AlignToPivot(&mut self) {
        loop {
            if self.m_Children.iter().any(|c| c.IsEnd()) {
                return;
            }

            let pivot = self
                .m_Children
                .iter()
                .map(|c| c.GetDocumentID())
                .max()
                .unwrap_or(NO_MORE_DOCS);

            let mut aligned = true;
            for c in &mut self.m_Children {
                if c.GetDocumentID() != pivot {
                    c.GoUntil(pivot, NO_MORE_DOCS);
                    if c.IsEnd() {
                        return;
                    }
                    if c.GetDocumentID() != pivot {
                        aligned = false;
                        break;
                    }
                }
            }

            if aligned {
                if self.m_Debug {
                    let ind = " ".repeat(self.m_DebugDepth * 2);
                    println!("{}AND match  doc {}", ind, pivot);
                }
                return;
            }
        }
    }
}

impl IndexReader for AndIndexReader {
    fn GoNext(&mut self) {
        if self.IsEnd() {
            return;
        }
        let doc = self.GetDocumentID();
        for c in &mut self.m_Children {
            if !c.IsEnd() && c.GetDocumentID() == doc {
                c.GoNext();
            }
        }
        self.AlignToPivot();
    }

    fn GoUntil(&mut self, target: u64, limit: u64) {
        for c in &mut self.m_Children {
            c.GoUntil(target, limit);
        }
        self.AlignToPivot();
    }

    fn IsEnd(&self) -> bool {
        self.m_Children.is_empty() || self.m_Children.iter().any(|c| c.IsEnd())
    }

    fn GetDocumentID(&self) -> u64 {
        if self.IsEnd() {
            NO_MORE_DOCS
        } else {
            self.m_Children[0].GetDocumentID()
        }
    }

    fn GetTermFreq(&self) -> u32 {
        self.m_Children.iter().map(|c| c.GetTermFreq()).sum()
    }

    fn GetScore(&mut self, entry: &DocDataEntry) -> f32 {
        self.m_Children.iter_mut().map(|c| c.GetScore(entry)).sum()
    }

    fn GetSourceMask(&mut self) -> u8 {
        let doc = self.GetDocumentID();
        self.m_Children
            .iter_mut()
            .filter(|c| !c.IsEnd() && c.GetDocumentID() == doc)
            .fold(0u8, |mask, c| mask | c.GetSourceMask())
    }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.m_Debug = true;
        self.m_DebugDepth = depth;
        println!("{}[AND]", " ".repeat(depth * 2));
        for c in &mut self.m_Children {
            c.SetDebug(label, depth + 1);
        }
    }

    fn Close(&mut self) {
        for child in &mut self.m_Children {
            child.Close();
        }
    }
}

/* ------------------------------------------------------------------ */

/*
* OrIndexReader — DAAT union with cached child document IDs and the set of
* children matching the current minimum document.
*/
#[allow(non_snake_case)]
pub struct OrIndexReader {
    m_Children: Vec<Box<dyn IndexReader>>,
    m_ChildDocs: Vec<u64>,
    m_MatchingChildren: Vec<usize>,
    m_CurrentDoc: u64,
    m_Debug: bool,
    m_DebugDepth: usize,
}

#[allow(non_snake_case)]
impl OrIndexReader {
    pub fn new(children: Vec<Box<dyn IndexReader>>) -> Self {
        let mut reader = Self {
            m_ChildDocs: vec![NO_MORE_DOCS; children.len()],
            m_Children: children,
            m_MatchingChildren: Vec::new(),
            m_CurrentDoc: NO_MORE_DOCS,
            m_Debug: false,
            m_DebugDepth: 0,
        };
        for index in 0..reader.m_Children.len() {
            reader.UpdateChildDoc(index);
        }
        reader.RefreshCurrentDoc();
        reader
    }

    fn UpdateChildDoc(&mut self, index: usize) {
        self.m_ChildDocs[index] = if self.m_Children[index].IsEnd() {
            NO_MORE_DOCS
        } else {
            self.m_Children[index].GetDocumentID()
        };
    }

    fn RefreshCurrentDoc(&mut self) {
        self.m_CurrentDoc = self
            .m_ChildDocs
            .iter()
            .copied()
            .min()
            .unwrap_or(NO_MORE_DOCS);
        self.m_MatchingChildren.clear();
        if self.m_CurrentDoc == NO_MORE_DOCS {
            return;
        }
        for (index, childDoc) in self.m_ChildDocs.iter().enumerate() {
            if *childDoc == self.m_CurrentDoc {
                self.m_MatchingChildren.push(index);
            }
        }
    }
}

impl IndexReader for OrIndexReader {
    fn GoNext(&mut self) {
        if self.IsEnd() {
            return;
        }
        let matching = self.m_MatchingChildren.clone();
        for index in matching {
            self.m_Children[index].GoNext();
            self.UpdateChildDoc(index);
        }
        self.RefreshCurrentDoc();
    }

    fn GoUntil(&mut self, target: u64, limit: u64) {
        for index in 0..self.m_Children.len() {
            if self.m_ChildDocs[index] < target {
                self.m_Children[index].GoUntil(target, limit);
                self.UpdateChildDoc(index);
            }
        }
        self.RefreshCurrentDoc();
    }

    fn IsEnd(&self) -> bool {
        self.m_CurrentDoc == NO_MORE_DOCS
    }

    fn GetDocumentID(&self) -> u64 {
        self.m_CurrentDoc
    }

    fn GetTermFreq(&self) -> u32 {
        self.m_MatchingChildren
            .iter()
            .map(|&index| self.m_Children[index].GetTermFreq())
            .sum()
    }

    fn GetScore(&mut self, entry: &DocDataEntry) -> f32 {
        let matching = self.m_MatchingChildren.clone();
        matching
            .into_iter()
            .map(|index| self.m_Children[index].GetScore(entry))
            .sum()
    }

    fn GetSourceMask(&mut self) -> u8 {
        let matching = self.m_MatchingChildren.clone();
        matching.into_iter().fold(0u8, |mask, index| {
            mask | self.m_Children[index].GetSourceMask()
        })
    }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.m_Debug = true;
        self.m_DebugDepth = depth;
        println!("{}[OR]", " ".repeat(depth * 2));
        for c in &mut self.m_Children {
            c.SetDebug(label, depth + 1);
        }
    }

    fn Close(&mut self) {
        for child in &mut self.m_Children {
            child.Close();
        }
    }
}

/* ------------------------------------------------------------------ */

#[allow(non_snake_case)]
pub struct WeakAndIndexReader {
    m_Children: Vec<Box<dyn IndexReader>>,
    m_ChildDocs: Vec<u64>,
    m_MatchingChildren: Vec<usize>,
    m_MinShouldMatch: u32,
    m_CurrentDoc: u64,
    m_Debug: bool,
    m_DebugDepth: usize,
}

#[allow(non_snake_case)]
impl WeakAndIndexReader {
    pub fn new(children: Vec<Box<dyn IndexReader>>, minShouldMatch: u32) -> Self {
        let mut reader = Self {
            m_ChildDocs: vec![NO_MORE_DOCS; children.len()],
            m_Children: children,
            m_MatchingChildren: Vec::new(),
            m_MinShouldMatch: minShouldMatch.max(1),
            m_CurrentDoc: NO_MORE_DOCS,
            m_Debug: false,
            m_DebugDepth: 0,
        };
        for index in 0..reader.m_Children.len() {
            reader.UpdateChildDoc(index);
        }
        reader.AlignToMatch();
        reader
    }

    fn UpdateChildDoc(&mut self, index: usize) {
        self.m_ChildDocs[index] = if self.m_Children[index].IsEnd() {
            NO_MORE_DOCS
        } else {
            self.m_Children[index].GetDocumentID()
        };
    }

    fn AlignToMatch(&mut self) {
        loop {
            let doc = self
                .m_ChildDocs
                .iter()
                .copied()
                .min()
                .unwrap_or(NO_MORE_DOCS);
            if doc == NO_MORE_DOCS {
                self.m_CurrentDoc = NO_MORE_DOCS;
                self.m_MatchingChildren.clear();
                return;
            }
            self.m_MatchingChildren = self
                .m_ChildDocs
                .iter()
                .enumerate()
                .filter_map(|(index, child_doc)| (*child_doc == doc).then_some(index))
                .collect();
            if self.m_MatchingChildren.len() >= self.m_MinShouldMatch as usize {
                self.m_CurrentDoc = doc;
                if self.m_Debug {
                    println!(
                        "{}WEAK-AND match doc {} children={}",
                        " ".repeat(self.m_DebugDepth * 2),
                        doc,
                        self.m_MatchingChildren.len()
                    );
                }
                return;
            }
            let matching = self.m_MatchingChildren.clone();
            for index in matching {
                self.m_Children[index].GoNext();
                self.UpdateChildDoc(index);
            }
        }
    }
}

impl IndexReader for WeakAndIndexReader {
    fn GoNext(&mut self) {
        if self.IsEnd() {
            return;
        }
        let matching = self.m_MatchingChildren.clone();
        for index in matching {
            self.m_Children[index].GoNext();
            self.UpdateChildDoc(index);
        }
        self.AlignToMatch();
    }
    fn GoUntil(&mut self, target: u64, limit: u64) {
        for index in 0..self.m_Children.len() {
            if self.m_ChildDocs[index] < target {
                self.m_Children[index].GoUntil(target, limit);
                self.UpdateChildDoc(index);
            }
        }
        self.AlignToMatch();
    }
    fn IsEnd(&self) -> bool {
        self.m_CurrentDoc == NO_MORE_DOCS
    }
    fn GetDocumentID(&self) -> u64 {
        self.m_CurrentDoc
    }
    fn GetTermFreq(&self) -> u32 {
        self.m_MatchingChildren
            .iter()
            .map(|&i| self.m_Children[i].GetTermFreq())
            .sum()
    }
    fn GetScore(&mut self, entry: &DocDataEntry) -> f32 {
        let matching = self.m_MatchingChildren.clone();
        matching
            .into_iter()
            .map(|i| self.m_Children[i].GetScore(entry))
            .sum()
    }
    fn GetSourceMask(&mut self) -> u8 {
        let matching = self.m_MatchingChildren.clone();
        matching
            .into_iter()
            .fold(0, |mask, i| mask | self.m_Children[i].GetSourceMask())
    }
    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.m_Debug = true;
        self.m_DebugDepth = depth;
        println!(
            "{}[WEAK-AND min={}]",
            " ".repeat(depth * 2),
            self.m_MinShouldMatch
        );
        for child in &mut self.m_Children {
            child.SetDebug(label, depth + 1);
        }
    }
    fn Close(&mut self) {
        for child in &mut self.m_Children {
            child.Close();
        }
    }
}

#[allow(non_snake_case)]
pub struct BoostIndexReader {
    m_Base: Box<dyn IndexReader>,
    m_Boost: Box<dyn IndexReader>,
    m_BoostWeight: f32,
    m_CurrentDoc: u64,
    m_BoostDoc: u64,
}

#[allow(non_snake_case)]
impl BoostIndexReader {
    pub fn new(base: Box<dyn IndexReader>, boost: Box<dyn IndexReader>, boostWeight: f32) -> Self {
        let current = if base.IsEnd() {
            NO_MORE_DOCS
        } else {
            base.GetDocumentID()
        };
        let boost_doc = if boost.IsEnd() {
            NO_MORE_DOCS
        } else {
            boost.GetDocumentID()
        };
        Self {
            m_Base: base,
            m_Boost: boost,
            m_BoostWeight: boostWeight,
            m_CurrentDoc: current,
            m_BoostDoc: boost_doc,
        }
    }

    fn BoostMatchesBase(&mut self) -> bool {
        if self.m_BoostDoc == NO_MORE_DOCS || self.IsEnd() {
            return false;
        }
        if self.m_BoostDoc < self.m_CurrentDoc {
            self.m_Boost.GoUntil(self.m_CurrentDoc, NO_MORE_DOCS);
            self.m_BoostDoc = if self.m_Boost.IsEnd() {
                NO_MORE_DOCS
            } else {
                self.m_Boost.GetDocumentID()
            };
        }
        self.m_BoostDoc == self.m_CurrentDoc
    }
}

impl IndexReader for BoostIndexReader {
    fn GoNext(&mut self) {
        if !self.IsEnd() {
            self.m_Base.GoNext();
            self.m_CurrentDoc = if self.m_Base.IsEnd() {
                NO_MORE_DOCS
            } else {
                self.m_Base.GetDocumentID()
            };
        }
    }
    fn GoUntil(&mut self, target: u64, limit: u64) {
        self.m_Base.GoUntil(target, limit);
        self.m_CurrentDoc = if self.m_Base.IsEnd() {
            NO_MORE_DOCS
        } else {
            self.m_Base.GetDocumentID()
        };
    }
    fn IsEnd(&self) -> bool {
        self.m_CurrentDoc == NO_MORE_DOCS
    }
    fn GetDocumentID(&self) -> u64 {
        self.m_CurrentDoc
    }
    fn GetTermFreq(&self) -> u32 {
        if self.IsEnd() {
            0
        } else {
            self.m_Base.GetTermFreq()
        }
    }
    fn GetScore(&mut self, entry: &DocDataEntry) -> f32 {
        if self.IsEnd() {
            return 0.0;
        }
        let base_score = self.m_Base.GetScore(entry);
        base_score
            + if self.BoostMatchesBase() {
                self.m_BoostWeight
            } else {
                0.0
            }
    }
    fn GetSourceMask(&mut self) -> u8 {
        if self.IsEnd() {
            return 0;
        }
        let base_mask = self.m_Base.GetSourceMask();
        base_mask
            | if self.BoostMatchesBase() {
                self.m_Boost.GetSourceMask()
            } else {
                0
            }
    }
    fn SetDebug(&mut self, label: &str, depth: usize) {
        println!(
            "{}[BOOST weight={}]",
            " ".repeat(depth * 2),
            self.m_BoostWeight
        );
        self.m_Base.SetDebug(label, depth + 1);
        self.m_Boost.SetDebug(label, depth + 1);
    }
    fn Close(&mut self) {
        self.m_Base.Close();
        self.m_Boost.Close();
        self.m_CurrentDoc = NO_MORE_DOCS;
        self.m_BoostDoc = NO_MORE_DOCS;
    }
}

/* ------------------------------------------------------------------ */

/*
* NotIndexReader — base reader filtered by an exclusion reader.
* SkipExcluded advances base past any doc also present in exclude.
*/
#[allow(non_snake_case)]
pub struct NotIndexReader {
    m_Base: Box<dyn IndexReader>,
    m_Exclude: Box<dyn IndexReader>,
    m_Debug: bool,
    m_DebugDepth: usize,
}

#[allow(non_snake_case)]
impl NotIndexReader {
    pub fn new(base: Box<dyn IndexReader>, exclude: Box<dyn IndexReader>) -> Self {
        let mut r = Self {
            m_Base: base,
            m_Exclude: exclude,
            m_Debug: false,
            m_DebugDepth: 0,
        };
        r.SkipExcluded();
        r
    }

    fn SkipExcluded(&mut self) {
        while !self.m_Base.IsEnd() {
            let doc = self.m_Base.GetDocumentID();
            self.m_Exclude.GoUntil(doc, NO_MORE_DOCS);
            if !self.m_Exclude.IsEnd() && self.m_Exclude.GetDocumentID() == doc {
                if self.m_Debug {
                    let ind = " ".repeat(self.m_DebugDepth * 2);
                    println!("{}NOT excluded  doc {}", ind, doc);
                }
                self.m_Base.GoNext();
            } else {
                break;
            }
        }
    }
}

impl IndexReader for NotIndexReader {
    fn GoNext(&mut self) {
        self.m_Base.GoNext();
        self.SkipExcluded();
    }
    fn GoUntil(&mut self, target: u64, limit: u64) {
        self.m_Base.GoUntil(target, limit);
        self.SkipExcluded();
    }
    fn IsEnd(&self) -> bool {
        self.m_Base.IsEnd()
    }
    fn GetDocumentID(&self) -> u64 {
        self.m_Base.GetDocumentID()
    }
    fn GetTermFreq(&self) -> u32 {
        self.m_Base.GetTermFreq()
    }

    fn GetScore(&mut self, entry: &DocDataEntry) -> f32 {
        self.m_Base.GetScore(entry)
    }

    fn GetSourceMask(&mut self) -> u8 {
        self.m_Base.GetSourceMask()
    }

    fn SetDebug(&mut self, label: &str, depth: usize) {
        self.m_Debug = true;
        self.m_DebugDepth = depth;
        let ind = " ".repeat(depth * 2);
        println!("{}[NOT]", ind);
        println!("{}  + base:", ind);
        self.m_Base.SetDebug(label, depth + 2);
        println!("{}  - excl:", ind);
        self.m_Exclude.SetDebug(label, depth + 2);
    }

    fn Close(&mut self) {
        self.m_Base.Close();
        self.m_Exclude.Close();
    }
}

#[allow(non_snake_case)]
pub struct VectorIndexReader {
    m_Results: Vec<VectorSearchResult>,
    m_Pos: usize,
}

#[allow(non_snake_case)]
impl VectorIndexReader {
    pub fn new(mut results: Vec<VectorSearchResult>) -> Self {
        results.sort_by_key(|result| result.doc_id);
        Self {
            m_Results: results,
            m_Pos: 0,
        }
    }
}

impl IndexReader for VectorIndexReader {
    fn GoNext(&mut self) {
        if !self.IsEnd() {
            self.m_Pos += 1;
        }
    }

    fn GoUntil(&mut self, target: u64, limit: u64) {
        while !self.IsEnd() && self.GetDocumentID() < target && self.GetDocumentID() < limit {
            self.m_Pos += 1;
        }
    }

    fn IsEnd(&self) -> bool {
        self.m_Pos >= self.m_Results.len()
    }

    fn GetDocumentID(&self) -> u64 {
        if self.IsEnd() {
            NO_MORE_DOCS
        } else {
            self.m_Results[self.m_Pos].doc_id
        }
    }

    fn GetScore(&mut self, _entry: &DocDataEntry) -> f32 {
        if self.IsEnd() {
            0.0
        } else {
            self.m_Results[self.m_Pos].score
        }
    }

    fn GetSourceMask(&mut self) -> u8 {
        if self.IsEnd() {
            0
        } else {
            READER_SOURCE_VECTOR
        }
    }

    fn Close(&mut self) {
        self.m_Pos = self.m_Results.len();
    }
}
