//! Insertion-ordered `MoMap` and `MoSet` matching `src/heap/table.c` semantics.
//!
//! - Preserves insertion order on iteration, `keys`, `values`, `entries`, `to_list`, and printing.
//! - Updating an existing key preserves its original insertion position.
//! - Removing a key preserves surviving order, using tombstones and compacting when sparse.

use mo_bytecode::types::TypeTable;

use crate::Value;
use crate::equal::value_equal;
use crate::hash::value_hash;

const EMPTY_SLOT: u32 = 0;
const TOMBSTONE_SLOT: u32 = u32::MAX;

#[derive(Clone)]
pub struct MoMap {
    pub key_type: u32,
    pub val_type: u32,
    entries: Vec<Option<(u64, Value, Value)>>,
    slots: Vec<u32>,
    active: usize,
    tombstones: usize,
}

impl MoMap {
    pub fn new(key_type: u32, val_type: u32) -> Self {
        Self {
            key_type,
            val_type,
            entries: Vec::new(),
            slots: Vec::new(),
            active: 0,
            tombstones: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.active
    }

    pub fn is_empty(&self) -> bool {
        self.active == 0
    }

    pub fn iter(&self) -> impl Iterator<Item = (&Value, &Value)> {
        self.entries
            .iter()
            .filter_map(|e| e.as_ref().map(|(_, k, v)| (k, v)))
    }

    fn find_slot(
        &self,
        types: &TypeTable,
        hash_key: [u64; 2],
        hash: u64,
        key: &Value,
    ) -> (Option<usize>, usize) {
        if self.slots.is_empty() {
            return (None, 0);
        }
        let mask = self.slots.len() - 1;
        let mut idx = (hash as usize) & mask;
        let mut first_tombstone = None;
        for _ in 0..self.slots.len() {
            let slot = self.slots[idx];
            if slot == EMPTY_SLOT {
                return (None, first_tombstone.unwrap_or(idx));
            }
            if slot == TOMBSTONE_SLOT {
                if first_tombstone.is_none() {
                    first_tombstone = Some(idx);
                }
            } else {
                let entry_idx = (slot - 1) as usize;
                if let Some((eh, ek, _)) = &self.entries[entry_idx]
                    && *eh == hash
                    && value_equal(types, hash_key, self.key_type, ek, key)
                {
                    return (Some(entry_idx), idx);
                }
            }
            idx = (idx + 1) & mask;
        }
        (None, first_tombstone.unwrap_or(0))
    }

    fn reindex(&mut self, compact: bool, new_slot_count: usize) {
        if compact {
            self.entries.retain(Option::is_some);
        }
        let slot_count = new_slot_count.max(8).next_power_of_two();
        self.slots = vec![EMPTY_SLOT; slot_count];
        self.tombstones = 0;
        let mask = slot_count - 1;
        for (i, entry) in self.entries.iter().enumerate() {
            let Some((h, _, _)) = entry else { continue };
            let mut idx = (*h as usize) & mask;
            while self.slots[idx] != EMPTY_SLOT {
                idx = (idx + 1) & mask;
            }
            self.slots[idx] = (i as u32) + 1;
        }
    }

    pub fn get(&self, types: &TypeTable, hash_key: [u64; 2], key: &Value) -> Option<&Value> {
        if self.active == 0 {
            return None;
        }
        let hash = value_hash(types, hash_key, self.key_type, key)?;
        let (found, _) = self.find_slot(types, hash_key, hash, key);
        found.and_then(|i| self.entries[i].as_ref().map(|(_, _, v)| v))
    }

    pub fn contains_key(&self, types: &TypeTable, hash_key: [u64; 2], key: &Value) -> bool {
        self.get(types, hash_key, key).is_some()
    }

    pub fn set(&mut self, types: &TypeTable, hash_key: [u64; 2], key: Value, val: Value) {
        let hash = value_hash(types, hash_key, self.key_type, &key).expect("hashable map key");
        if self.slots.is_empty() || (self.active + self.tombstones + 1) * 2 > self.slots.len() {
            let want = ((self.active + 1) * 2).max(8);
            self.reindex(self.entries.len() > self.active * 2, want);
        }
        let (found, slot_idx) = self.find_slot(types, hash_key, hash, &key);
        if let Some(entry_idx) = found {
            if let Some(entry) = &mut self.entries[entry_idx] {
                entry.2 = val;
            }
        } else {
            if self.slots[slot_idx] == TOMBSTONE_SLOT {
                self.tombstones -= 1;
            }
            let entry_idx = self.entries.len();
            self.entries.push(Some((hash, key, val)));
            self.slots[slot_idx] = (entry_idx as u32) + 1;
            self.active += 1;
        }
    }

    pub fn remove(&mut self, types: &TypeTable, hash_key: [u64; 2], key: &Value) -> bool {
        if self.active == 0 {
            return false;
        }
        let Some(hash) = value_hash(types, hash_key, self.key_type, key) else {
            return false;
        };
        let (found, slot_idx) = self.find_slot(types, hash_key, hash, key);
        let Some(entry_idx) = found else {
            return false;
        };
        self.entries[entry_idx] = None;
        self.slots[slot_idx] = TOMBSTONE_SLOT;
        self.active -= 1;
        self.tombstones += 1;
        if self.entries.len() > 16 && self.active * 2 < self.entries.len() {
            let want = (self.active * 2).max(8);
            self.reindex(true, want);
        }
        true
    }

    pub fn drain_values(&mut self, out: &mut Vec<Value>) {
        for (_, k, v) in self.entries.drain(..).flatten() {
            out.push(k);
            out.push(v);
        }
        self.slots.clear();
        self.active = 0;
        self.tombstones = 0;
    }
}

#[derive(Clone)]
pub struct MoSet {
    pub elem_type: u32,
    inner: MoMap,
}

impl MoSet {
    pub fn new(elem_type: u32) -> Self {
        Self {
            elem_type,
            inner: MoMap::new(elem_type, elem_type),
        }
    }

    pub fn len(&self) -> usize {
        self.inner.len()
    }

    pub fn is_empty(&self) -> bool {
        self.inner.is_empty()
    }

    pub fn iter(&self) -> impl Iterator<Item = &Value> {
        self.inner.iter().map(|(k, _)| k)
    }

    pub fn contains(&self, types: &TypeTable, hash_key: [u64; 2], elem: &Value) -> bool {
        self.inner.contains_key(types, hash_key, elem)
    }

    pub fn add(&mut self, types: &TypeTable, hash_key: [u64; 2], elem: Value) {
        self.inner.set(types, hash_key, elem, Value::Unit);
    }

    pub fn remove(&mut self, types: &TypeTable, hash_key: [u64; 2], elem: &Value) -> bool {
        self.inner.remove(types, hash_key, elem)
    }

    pub fn drain_values(&mut self, out: &mut Vec<Value>) {
        self.inner.drain_values(out);
    }
}
