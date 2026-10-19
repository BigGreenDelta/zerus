# Rust license notices for crates that publish none

`scripts/collect-licenses.py` takes a crate's notices from its crates.io
package. These crates ship no license file there, so their notices come from
the upstream repository instead, copied unchanged at the commit below. Before
upgrading one of them, compare the new upstream file and update the copy, the
URL and the checksum.

| Crate | Source | SHA-256 |
|---|---|---|
| `append-only-bytes` | https://github.com/zxch3n/append-only-bytes/blob/5ee04bd3e3249c6bfeb9c3d31644ada87bb90cbe/LICENSE | `a9ec532f4f11817f427e4f3ca0432b176138721e0866f152864b17ebdeac6bbf` |
| `arref` | https://github.com/zxch3n/arref/blob/c09551ed17509719e8f9a46d2814c4378f8786fa/LICENSE | `a9ec532f4f11817f427e4f3ca0432b176138721e0866f152864b17ebdeac6bbf` |
| `generic-btree` | https://github.com/loro-dev/generic-btree/blob/0ae02fad39fbaf6b4836e1e578e3da7759e321ed/LICENSE | `732d8393fd664a8126a46f0b7a4c23c577fdfc9658e9ad1620d5354050fb9f51` |
| `serde_columnar` | https://github.com/loro-dev/columnar/blob/06663cfc569c6c770b28688f015f8a87fc04e156/LICENSE | `a9af0e3a3ded007f9914a1125b736ec526d469105c6577e367e2a9ca4de39d6b` |
| `serde_columnar_derive` | https://github.com/loro-dev/columnar/blob/06663cfc569c6c770b28688f015f8a87fc04e156/LICENSE | `a9af0e3a3ded007f9914a1125b736ec526d469105c6577e367e2a9ca4de39d6b` |
| `uniffi` | https://github.com/mozilla/uniffi-rs/blob/7bc621ee9d0d15371d792612707ba5ed4111f950/LICENSE | `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5` |
| `uniffi_bindgen` | https://github.com/mozilla/uniffi-rs/blob/7bc621ee9d0d15371d792612707ba5ed4111f950/LICENSE | `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5` |
| `uniffi_core` | https://github.com/mozilla/uniffi-rs/blob/7bc621ee9d0d15371d792612707ba5ed4111f950/LICENSE | `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5` |
| `uniffi_internal_macros` | https://github.com/mozilla/uniffi-rs/blob/7bc621ee9d0d15371d792612707ba5ed4111f950/LICENSE | `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5` |
| `uniffi_macros` | https://github.com/mozilla/uniffi-rs/blob/7bc621ee9d0d15371d792612707ba5ed4111f950/LICENSE | `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5` |
| `uniffi_meta` | https://github.com/mozilla/uniffi-rs/blob/7bc621ee9d0d15371d792612707ba5ed4111f950/LICENSE | `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5` |
| `uniffi_pipeline` | https://github.com/mozilla/uniffi-rs/blob/7bc621ee9d0d15371d792612707ba5ed4111f950/LICENSE | `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5` |
| `uniffi_udl` | https://github.com/mozilla/uniffi-rs/blob/7bc621ee9d0d15371d792612707ba5ed4111f950/LICENSE | `1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5` |

`serde_columnar` and `serde_columnar_derive` are offered under "MIT OR
Apache-2.0"; Zerus uses them under MIT, the license their repository carries.

The `uniffi` crates (UniFFI 0.32.2, tag `v0.32.2`) are under MPL-2.0; the Android
app's task replica links them (`crates/replica-ffi`). `uniffi_bindgen` and
`uniffi_udl` are built only with that crate's `bindgen` feature, for the binding
generator: the app's library never contains them, so its notices list them only
when a collection includes the generator.
