use std::collections::VecDeque;

use mo_bytecode::TypeKind;
use mo_bytecode::types::TypeTable;
use mo_def::PrimId;
use mo_def::limits::MO_LIMIT_COLLECTION_SIZE;
use mo_heap::decimal::decimal_add;
use mo_heap::equal::value_equal;
use mo_heap::table::{MoMap, MoSet};
use mo_heap::{Heap, Value};

use crate::{PrimError, check_value_bytes_fit};

pub fn call(
    heap: &Heap,
    types: &TypeTable,
    prim: PrimId,
    params: &[u32],
    result_ty: u32,
    args: &mut [Value],
) -> Result<Value, PrimError> {
    match prim {
        // Range primitives
        PrimId::RangeContains => {
            let (start, end) = args[0].as_range();
            let n = args[1].as_int();
            Ok(Value::Bool(start <= n && n < end))
        }
        PrimId::RangeToList => {
            let (start, end) = args[0].as_range();
            let count = if end > start {
                i128::from(end) - i128::from(start)
            } else {
                0
            };
            if count > i128::from(MO_LIMIT_COLLECTION_SIZE) {
                return Err(PrimError::OutOfMemory(None));
            }
            let mut items = VecDeque::with_capacity(count as usize);
            let mut n = start;
            while n < end {
                items.push_back(Value::Int(n));
                n += 1;
            }
            Ok(heap.alloc_list(items)?)
        }

        // List primitives
        PrimId::ListEmpty => Ok(heap.alloc_list(VecDeque::new())?),
        PrimId::ListSize => Ok(Value::Int(args[0].as_list().len() as i64)),
        PrimId::ListIsEmpty => Ok(Value::Bool(args[0].as_list().is_empty())),
        PrimId::ListGet => {
            let list = args[0].as_list();
            let idx = args[1].as_int();
            let opt = if idx >= 0 && (idx as usize) < list.len() {
                Some(list[idx as usize].clone())
            } else {
                None
            };
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::ListFirst => {
            let opt = args[0].as_list().front().cloned();
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::ListLast => {
            let opt = args[0].as_list().back().cloned();
            Ok(heap.make_option(types, result_ty, opt)?)
        }
        PrimId::ListAppend => {
            let [list, elem, ..] = args else {
                unreachable!()
            };
            Ok(heap.list_append(list, elem)?)
        }
        PrimId::ListPrepend => {
            let [list, elem, ..] = args else {
                unreachable!()
            };
            Ok(heap.list_prepend(list, elem)?)
        }
        PrimId::ListConcat => {
            let [lhs, rhs, ..] = args else { unreachable!() };
            Ok(heap.list_concat(lhs, rhs)?)
        }
        PrimId::ListReverse => Ok(heap.list_reverse(&mut args[0])?),
        PrimId::ListTake | PrimId::ListDrop => {
            let count = args[1].as_int();
            if count < 0 {
                return Err(PrimError::Outside(if prim == PrimId::ListTake {
                    "List.take with a negative count"
                } else {
                    "List.drop with a negative count"
                }));
            }
            let len = args[0].as_list().len();
            let n = (count as u64).min(len as u64) as usize;
            let (start, take_len) = if prim == PrimId::ListTake {
                (0, n)
            } else {
                (n, len - n)
            };
            Ok(heap.list_slice(&mut args[0], start, take_len)?)
        }
        PrimId::ListSet => {
            let idx = args[1].as_int();
            let len = args[0].as_list().len();
            if idx < 0 || (idx as usize) >= len {
                return Ok(heap.make_option(types, result_ty, None)?);
            }
            let [list, _, elem, ..] = args else {
                unreachable!()
            };
            let updated = heap.list_set(list, idx as usize, elem)?;
            Ok(heap.make_option(types, result_ty, Some(updated))?)
        }
        PrimId::ListRemoveAt => {
            let idx = args[1].as_int();
            let len = args[0].as_list().len();
            if idx < 0 || (idx as usize) >= len {
                return Ok(heap.make_option(types, result_ty, None)?);
            }
            let updated = heap.list_remove_at(&mut args[0], idx as usize)?;
            Ok(heap.make_option(types, result_ty, Some(updated))?)
        }
        PrimId::ListEnumerate => {
            let pair_ty = types.get(result_ty).parameters[0];
            #[expect(
                clippy::expect_used,
                reason = "verified bytecode guarantees tuple layout exists"
            )]
            let tuple_layout = types
                .find_tuple_layout(pair_ty)
                .expect("tuple layout for enumerate");
            let list = args[0].as_list();
            let mut out = VecDeque::with_capacity(list.len());
            for (i, item) in list.iter().enumerate() {
                let pair = heap.construct(
                    types,
                    tuple_layout,
                    vec![Value::Int(i as i64), item.clone()],
                )?;
                out.push_back(pair);
            }
            Ok(heap.alloc_list(out)?)
        }
        PrimId::ListZip => {
            let pair_ty = types.get(result_ty).parameters[0];
            #[expect(
                clippy::expect_used,
                reason = "verified bytecode guarantees tuple layout exists"
            )]
            let tuple_layout = types
                .find_tuple_layout(pair_ty)
                .expect("tuple layout for zip");
            let a = args[0].as_list();
            let b = args[1].as_list();
            let len = a.len().min(b.len());
            let mut out = VecDeque::with_capacity(len);
            for i in 0..len {
                let pair = heap.construct(types, tuple_layout, vec![a[i].clone(), b[i].clone()])?;
                out.push_back(pair);
            }
            Ok(heap.alloc_list(out)?)
        }
        PrimId::ListContains => {
            let elem_ty = types.get(params[0]).parameters[0];
            let list = args[0].as_list();
            let needle = &args[1];
            let found = list
                .iter()
                .any(|item| value_equal(types, heap.hash_key(), elem_ty, item, needle));
            Ok(Value::Bool(found))
        }
        PrimId::ListUnique => {
            let elem_ty = types.get(result_ty).parameters[0];
            let mut seen = MoSet::new(elem_ty);
            let mut out = VecDeque::new();
            for item in args[0].as_list() {
                if !seen.contains(types, heap.hash_key(), item) {
                    seen.add(types, heap.hash_key(), item.clone());
                    out.push_back(item.clone());
                }
            }
            Ok(heap.alloc_list(out)?)
        }
        PrimId::ListToSet => {
            let elem_ty = types.get(result_ty).parameters[0];
            let mut set = MoSet::new(elem_ty);
            for item in args[0].as_list() {
                set.add(types, heap.hash_key(), item.clone());
            }
            Ok(heap.alloc_set(set)?)
        }
        PrimId::ListToMap => {
            let map_t = types.get(result_ty);
            let mut map = MoMap::new(map_t.parameters[0], map_t.parameters[1]);
            for pair in args[0].as_list() {
                let k = heap.extract_field(pair, 0);
                let v = heap.extract_field(pair, 1);
                map.set(types, heap.hash_key(), k, v);
            }
            Ok(heap.alloc_map(map)?)
        }
        PrimId::ListSum => {
            let list = args[0].as_list();
            match types.get(result_ty).kind {
                TypeKind::Int => {
                    let mut acc: i64 = 0;
                    for item in list {
                        acc = acc
                            .checked_add(item.as_int())
                            .ok_or(PrimError::Overflow("Int"))?;
                    }
                    Ok(Value::Int(acc))
                }
                TypeKind::Decimal => {
                    let mut acc: i128 = 0;
                    for item in list {
                        acc = decimal_add(acc, item.as_decimal())
                            .ok_or(PrimError::Overflow("Decimal"))?;
                    }
                    Ok(Value::Decimal(acc))
                }
                TypeKind::Float => {
                    let mut acc: f64 = 0.0;
                    for item in list {
                        acc += item.as_float();
                    }
                    if acc.is_nan() {
                        acc = f64::from_bits(0x7ff8_0000_0000_0000);
                    }
                    Ok(Value::Float(acc))
                }
                TypeKind::BigInt => crate::bigint::sum_list(heap, list),
                _ => unreachable!("invalid List.sum result type"),
            }
        }
        PrimId::ListJoin => {
            let list = args[0].as_list();
            let sep = args[1].as_text();
            let mut total = 0usize;
            for (i, item) in list.iter().enumerate() {
                total = total.saturating_add(item.as_text().len());
                if i != 0 {
                    total = total.saturating_add(sep.len());
                }
                check_value_bytes_fit(total)?;
            }
            let mut out = String::with_capacity(total);
            for (i, item) in list.iter().enumerate() {
                if i != 0 {
                    out.push_str(sep);
                }
                out.push_str(item.as_text());
            }
            Ok(heap.alloc_text(out)?)
        }

        // Map primitives
        PrimId::MapEmpty => {
            let t = types.get(result_ty);
            Ok(heap.alloc_map(MoMap::new(t.parameters[0], t.parameters[1]))?)
        }
        PrimId::MapSize => Ok(Value::Int(args[0].as_map().len() as i64)),
        PrimId::MapIsEmpty => Ok(Value::Bool(args[0].as_map().is_empty())),
        PrimId::MapGet => {
            let found = args[0]
                .as_map()
                .get(types, heap.hash_key(), &args[1])
                .cloned();
            Ok(heap.make_option(types, result_ty, found)?)
        }
        PrimId::MapHas => Ok(Value::Bool(args[0].as_map().contains_key(
            types,
            heap.hash_key(),
            &args[1],
        ))),
        PrimId::MapSet | PrimId::MapLiteralInsert => {
            if prim == PrimId::MapLiteralInsert
                && args[0]
                    .as_map()
                    .contains_key(types, heap.hash_key(), &args[1])
            {
                return Err(PrimError::RepeatedKey(
                    "two keys of a map literal that turn out equal",
                ));
            }
            let [map, key, val, ..] = args else {
                unreachable!()
            };
            Ok(heap.map_set(types, map, key, val)?)
        }
        PrimId::MapRemove => {
            let [map, key, ..] = args else { unreachable!() };
            Ok(heap.map_remove(types, map, key)?)
        }
        PrimId::MapKeys => {
            let items: VecDeque<Value> = args[0].as_map().iter().map(|(k, _)| k.clone()).collect();
            Ok(heap.alloc_list(items)?)
        }
        PrimId::MapValues => {
            let items: VecDeque<Value> = args[0].as_map().iter().map(|(_, v)| v.clone()).collect();
            Ok(heap.alloc_list(items)?)
        }
        PrimId::MapEntries => {
            let pair_ty = types.get(result_ty).parameters[0];
            #[expect(
                clippy::expect_used,
                reason = "verified bytecode guarantees tuple layout exists"
            )]
            let tuple_layout = types
                .find_tuple_layout(pair_ty)
                .expect("tuple layout for Map.entries");
            let mut items = VecDeque::with_capacity(args[0].as_map().len());
            for (k, v) in args[0].as_map().iter() {
                let pair = heap.construct(types, tuple_layout, vec![k.clone(), v.clone()])?;
                items.push_back(pair);
            }
            Ok(heap.alloc_list(items)?)
        }

        // Set primitives
        PrimId::SetEmpty => {
            let t = types.get(result_ty);
            Ok(heap.alloc_set(MoSet::new(t.parameters[0]))?)
        }
        PrimId::SetSize => Ok(Value::Int(args[0].as_set().len() as i64)),
        PrimId::SetIsEmpty => Ok(Value::Bool(args[0].as_set().is_empty())),
        PrimId::SetHas => Ok(Value::Bool(args[0].as_set().contains(
            types,
            heap.hash_key(),
            &args[1],
        ))),
        PrimId::SetAdd => {
            let [set, elem, ..] = args else {
                unreachable!()
            };
            Ok(heap.set_add(types, set, elem)?)
        }
        PrimId::SetRemove => {
            let [set, elem, ..] = args else {
                unreachable!()
            };
            Ok(heap.set_remove(types, set, elem)?)
        }
        PrimId::SetUnion => {
            let [left, right, ..] = args else {
                unreachable!()
            };
            Ok(heap.set_union(types, left, right)?)
        }
        PrimId::SetIntersection => {
            let elem_ty = types.get(result_ty).parameters[0];
            let mut out = MoSet::new(elem_ty);
            let rhs = args[1].as_set();
            for item in args[0].as_set().iter() {
                if rhs.contains(types, heap.hash_key(), item) {
                    out.add(types, heap.hash_key(), item.clone());
                }
            }
            Ok(heap.alloc_set(out)?)
        }
        PrimId::SetDifference => {
            let elem_ty = types.get(result_ty).parameters[0];
            let mut out = MoSet::new(elem_ty);
            let rhs = args[1].as_set();
            for item in args[0].as_set().iter() {
                if !rhs.contains(types, heap.hash_key(), item) {
                    out.add(types, heap.hash_key(), item.clone());
                }
            }
            Ok(heap.alloc_set(out)?)
        }
        PrimId::SetToList => {
            let items: VecDeque<Value> = args[0].as_set().iter().cloned().collect();
            Ok(heap.alloc_list(items)?)
        }

        _ => unreachable!("unexpected collection primitive {prim:?}"),
    }
}
