//! Densely numbered entity references as mapping keys.
use std::marker::PhantomData;
use std::ops::{Deref, DerefMut, Index, IndexMut};
use std::slice;
use std::vec::Vec;
use {EntityRef, Iter, IterMut, Keys};

/// A primary mapping `K -> V` allocating dense entity references.
///
/// The `PrimaryMap` data structure uses the dense index space to implement a map with a vector.
///
/// A primary map contains the main definition of an entity, and it can be used to allocate new
/// entity references with the `push` method.
///
/// There should only be a single `PrimaryMap` instance for a given `EntityRef` type, otherwise
/// conflicting references will be created. Using unknown keys for indexing will cause a panic.
#[derive(Debug, Clone)]
pub struct PrimaryMap<K, V>
where
    K: EntityRef,
{
    elems: Vec<V>,
    unused: PhantomData<K>,
}

impl<K, V> PrimaryMap<K, V>
where
    K: EntityRef,
{
    /// Create a new empty map.
    pub fn new() -> Self {
        Self {
            elems: Vec::new(),
            unused: PhantomData,
        }
    }

    /// Check if `k` is a valid key in the map.
    pub fn is_valid(&self, k: K) -> bool {
        k.index() < self.elems.len()
    }

    /// Get the element at `k` if it exists.
    pub fn get(&self, k: K) -> Option<&V> {
        self.elems.get(k.index())
    }

    /// Is this map completely empty?
    pub fn is_empty(&self) -> bool {
        self.elems.is_empty()
    }

    /// Get the total number of entity references created.
    pub fn len(&self) -> usize {
        self.elems.len()
    }

    /// Iterate over all the keys in this map.
    pub fn keys(&self) -> Keys<K> {
        Keys::with_len(self.elems.len())
    }

    /// Iterate over all the values in this map.
    pub fn values(&self) -> slice::Iter<V> {
        self.elems.iter()
    }

    /// Iterate over all the values in this map, mutable edition.
    pub fn values_mut(&mut self) -> slice::IterMut<V> {
        self.elems.iter_mut()
    }

    /// Iterate over all the keys and values in this map.
    pub fn iter(&self) -> Iter<K, V> {
        Iter::new(self.elems.iter())
    }

    /// Iterate over all the keys and values in this map, mutable edition.
    pub fn iter_mut(&mut self) -> IterMut<K, V> {
        IterMut::new(self.elems.iter_mut())
    }

    /// Remove all entries from this map.
    pub fn clear(&mut self) {
        self.elems.clear()
    }

    /// Get the key that will be assigned to the next pushed value.
    pub fn next_key(&self) -> K {
        K::new(self.elems.len())
    }

    /// Append `v` to the mapping, assigning a new key which is returned.
    pub fn push(&mut self, v: V) -> K {
        let k = self.next_key();
        self.elems.push(v);
        k
    }
}

/// Immutable indexing into an `PrimaryMap`.
/// The indexed value must be in the map.
impl<K, V> Index<K> for PrimaryMap<K, V>
where
    K: EntityRef,
{
    type Output = V;

    fn index(&self, k: K) -> &V {
        &self.elems[k.index()]
    }
}

/// Mutable indexing into an `PrimaryMap`.
impl<K, V> IndexMut<K> for PrimaryMap<K, V>
where
    K: EntityRef,
{
    fn index_mut(&mut self, k: K) -> &mut V {
        &mut self.elems[k.index()]
    }
}

impl<'a, K, V> IntoIterator for &'a PrimaryMap<K, V>
where
    K: EntityRef,
{
    type Item = (K, &'a V);
    type IntoIter = Iter<'a, K, V>;

    fn into_iter(self) -> Self::IntoIter {
        Iter::new(self.elems.iter())
    }
}

impl<'a, K, V> IntoIterator for &'a mut PrimaryMap<K, V>
where
    K: EntityRef,
{
    type Item = (K, &'a mut V);
    type IntoIter = IterMut<'a, K, V>;

    fn into_iter(self) -> Self::IntoIter {
        IterMut::new(self.elems.iter_mut())
    }
}

impl<K, V> Deref for PrimaryMap<K, V>
where
    K: EntityRef,
{
    type Target = [V];

    fn deref(&self) -> &Self::Target {
        &self.elems
    }
}

impl<K, V> DerefMut for PrimaryMap<K, V>
where
    K: EntityRef,
{
    fn deref_mut(&mut self) -> &mut [V] {
        &mut self.elems
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // `EntityRef` impl for testing.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    struct E(u32);

    impl EntityRef for E {
        fn new(i: usize) -> Self {
            E(i as u32)
        }
        fn index(self) -> usize {
            self.0 as usize
        }
    }

    #[test]
    fn basic() {
        let r0 = E(0);
        let r1 = E(1);
        let m = PrimaryMap::<E, isize>::new();

        let v: Vec<E> = m.keys().collect();
        assert_eq!(v, []);

        assert!(!m.is_valid(r0));
        assert!(!m.is_valid(r1));
    }

    #[test]
    fn push() {
        let mut m = PrimaryMap::new();
        let k0: E = m.push(12);
        let k1 = m.push(33);

        assert_eq!(m[k0], 12);
        assert_eq!(m[k1], 33);

        let v: Vec<E> = m.keys().collect();
        assert_eq!(v, [k0, k1]);
    }

    #[test]
    fn iter() {
        let mut m: PrimaryMap<E, usize> = PrimaryMap::new();
        m.push(12);
        m.push(33);

        let mut i = 0;
        for (key, value) in &m {
            assert_eq!(key.index(), i);
            match i {
                0 => assert_eq!(*value, 12),
                1 => assert_eq!(*value, 33),
                _ => panic!(),
            }
            i += 1;
        }
        i = 0;
        for (key_mut, value_mut) in m.iter_mut() {
            assert_eq!(key_mut.index(), i);
            match i {
                0 => assert_eq!(*value_mut, 12),
                1 => assert_eq!(*value_mut, 33),
                _ => panic!(),
            }
            i += 1;
        }
    }

    #[test]
    fn iter_rev() {
        let mut m: PrimaryMap<E, usize> = PrimaryMap::new();
        m.push(12);
        m.push(33);

        let mut i = 2;
        for (key, value) in m.iter().rev() {
            i -= 1;
            assert_eq!(key.index(), i);
            match i {
                0 => assert_eq!(*value, 12),
                1 => assert_eq!(*value, 33),
                _ => panic!(),
            }
        }

        i = 2;
        for (key, value) in m.iter_mut().rev() {
            i -= 1;
            assert_eq!(key.index(), i);
            match i {
                0 => assert_eq!(*value, 12),
                1 => assert_eq!(*value, 33),
                _ => panic!(),
            }
        }
    }
    #[test]
    fn keys() {
        let mut m: PrimaryMap<E, usize> = PrimaryMap::new();
        m.push(12);
        m.push(33);

        let mut i = 0;
        for key in m.keys() {
            assert_eq!(key.index(), i);
            i += 1;
        }
    }

    #[test]
    fn keys_rev() {
        let mut m: PrimaryMap<E, usize> = PrimaryMap::new();
        m.push(12);
        m.push(33);

        let mut i = 2;
        for key in m.keys().rev() {
            i -= 1;
            assert_eq!(key.index(), i);
        }
    }

    #[test]
    fn values() {
        let mut m: PrimaryMap<E, usize> = PrimaryMap::new();
        m.push(12);
        m.push(33);

        let mut i = 0;
        for value in m.values() {
            match i {
                0 => assert_eq!(*value, 12),
                1 => assert_eq!(*value, 33),
                _ => panic!(),
            }
            i += 1;
        }
        i = 0;
        for value_mut in m.values_mut() {
            match i {
                0 => assert_eq!(*value_mut, 12),
                1 => assert_eq!(*value_mut, 33),
                _ => panic!(),
            }
            i += 1;
        }
    }

    #[test]
    fn values_rev() {
        let mut m: PrimaryMap<E, usize> = PrimaryMap::new();
        m.push(12);
        m.push(33);

        let mut i = 2;
        for value in m.values().rev() {
            i -= 1;
            match i {
                0 => assert_eq!(*value, 12),
                1 => assert_eq!(*value, 33),
                _ => panic!(),
            }
        }
        i = 2;
        for value_mut in m.values_mut().rev() {
            i -= 1;
            match i {
                0 => assert_eq!(*value_mut, 12),
                1 => assert_eq!(*value_mut, 33),
                _ => panic!(),
            }
        }
    }

    #[test]
    fn deref() {
        let mut m = PrimaryMap::<E, isize>::new();
        let _: &[isize] = m.as_ref();
        let _: &mut [isize] = m.as_mut();
        let _: &[isize] = &m;
        let _: &mut [isize] = &mut m;
    }
}
