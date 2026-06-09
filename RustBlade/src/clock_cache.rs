use std::collections::HashMap;
use std::hash::Hash;

// ---------------------------------------------------------------------------
// Clock-replacement cache.
//
// Mirrors REF's `ClockPolicy` + `PostingListCache` frame array.
//
// Each frame carries a "recently used" bit.  On eviction the clock hand
// sweeps forward, clearing the bit on touched frames and evicting the first
// frame found with the bit already clear (i.e. not recently used).
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq)]
enum FrameState {
    /// Frame is empty or evictable.
    Available,
    /// Frame was accessed since the last sweep; give it another pass.
    Touched,
}

struct Frame<K, V> {
    key:   Option<K>,
    value: Option<V>,
    state: FrameState,
}

impl<K, V> Frame<K, V> {
    fn empty() -> Self {
        Self { key: None, value: None, state: FrameState::Available }
    }
}

pub struct ClockCache<K, V> {
    capacity: usize,
    frames:   Vec<Frame<K, V>>,
    /// Maps a key to the frame index that holds it.
    index:    HashMap<K, usize>,
    /// Clock hand — next frame to examine during eviction.
    hand:     usize,
}

impl<K: Eq + Hash + Clone, V: Clone> ClockCache<K, V> {
    pub fn new(capacity: usize) -> Self {
        assert!(capacity > 0, "cache capacity must be > 0");
        let frames = (0..capacity).map(|_| Frame::empty()).collect();
        Self { capacity, frames, index: HashMap::new(), hand: 0 }
    }

    /// Look up `key` and mark the frame as recently used on hit.
    pub fn get(&mut self, key: &K) -> Option<V> {
        if let Some(&fi) = self.index.get(key) {
            self.frames[fi].state = FrameState::Touched;
            self.frames[fi].value.clone()
        } else {
            None
        }
    }

    /// Insert `(key, value)`.  If `key` is already cached, update in place.
    pub fn insert(&mut self, key: K, value: V) {
        if let Some(&fi) = self.index.get(&key) {
            self.frames[fi].value = Some(value);
            self.frames[fi].state = FrameState::Touched;
            return;
        }
        let victim = self.evict_one();
        self.frames[victim] = Frame {
            key:   Some(key.clone()),
            value: Some(value),
            state: FrameState::Touched,
        };
        self.index.insert(key, victim);
    }

    pub fn contains(&self, key: &K) -> bool {
        self.index.contains_key(key)
    }

    pub fn len(&self) -> usize {
        self.index.len()
    }

    // -- private ------------------------------------------------------------

    /// Advance the clock hand until an Available frame is found.
    /// Touched frames get one reprieve (bit cleared, hand moves on).
    fn evict_one(&mut self) -> usize {
        loop {
            let fi = self.hand;
            self.hand = (self.hand + 1) % self.capacity;

            match self.frames[fi].state {
                FrameState::Available => {
                    // Evict: remove old key from index map.
                    if let Some(old_key) = self.frames[fi].key.take() {
                        self.index.remove(&old_key);
                    }
                    self.frames[fi].value = None;
                    return fi;
                }
                FrameState::Touched => {
                    // Second-chance: clear the bit and continue.
                    self.frames[fi].state = FrameState::Available;
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn basic_insert_get() {
        let mut c: ClockCache<u32, &str> = ClockCache::new(3);
        c.insert(1, "a");
        c.insert(2, "b");
        c.insert(3, "c");
        assert_eq!(c.get(&1), Some("a"));
        assert_eq!(c.get(&2), Some("b"));
        assert_eq!(c.get(&3), Some("c"));
    }

    #[test]
    fn eviction_under_capacity() {
        let mut c: ClockCache<u32, u32> = ClockCache::new(2);
        c.insert(1, 10);
        c.insert(2, 20);
        c.insert(3, 30); // should evict one of 1 or 2
        // At least the newest entry must be present.
        assert_eq!(c.get(&3), Some(30));
    }
}
