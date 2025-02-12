/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::slice;
use std::ptr;
use std::cell::UnsafeCell;
use std::path::PathBuf;
use std::ffi::OsString;
use std::fs::File;
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::os::windows::io::IntoRawHandle;
use std::path::Path;

use comptr::ComPtr;

use winapi::Interface;
use winapi::ctypes::c_void;
use winapi::um::dwrite::{IDWriteFontFace, IDWriteFontFile, IDWriteFontFileStream};
use winapi::um::dwrite::{IDWriteFontFileLoader, IDWriteLocalFontFileLoader};
use winapi::um::dwrite::{DWRITE_FONT_SIMULATIONS, DWRITE_FONT_FACE_TYPE_UNKNOWN};
use winapi::um::dwrite::{DWRITE_FONT_FACE_TYPE, DWRITE_FONT_FILE_TYPE_UNKNOWN};

use font_file_loader_impl::DataFontHelper;
use font_face::FontFace;
use super::DWriteFactory;

pub struct FontFile {
    native: UnsafeCell<ComPtr<IDWriteFontFile>>,
    data_key: usize,
    face_type: DWRITE_FONT_FACE_TYPE,
}

impl FontFile {
    pub fn new_from_path<P>(path: P) -> Option<FontFile> where P: AsRef<Path> {
        unsafe {
            let mut path: Vec<u16> = path.as_ref().as_os_str().encode_wide().collect();
            path.push(0);

            let mut font_file: ComPtr<IDWriteFontFile> = ComPtr::new();
            let hr = (*DWriteFactory()).CreateFontFileReference(path.as_ptr(),
                                                                ptr::null(),
                                                                font_file.getter_addrefs());
            if hr != 0 || font_file.is_null() {
                return None
            }

            Some(FontFile {
                native: UnsafeCell::new(font_file),
                data_key: 0,
                face_type: DWRITE_FONT_FACE_TYPE_UNKNOWN,
            })
        }
    }

    pub fn new_from_data(data: &[u8]) -> Option<FontFile> {
        let (font_file, key) = DataFontHelper::register_font_data(data);

        let mut ff = FontFile {
            native: UnsafeCell::new(font_file),
            data_key: key,
            face_type: DWRITE_FONT_FACE_TYPE_UNKNOWN,
        };

        if ff.analyze() == 0 {
            DataFontHelper::unregister_font_data(key);
            return None;
        }

        Some(ff)
    }

    pub fn analyze_data(data: &[u8]) -> u32 {
        let (font_file, key) = DataFontHelper::register_font_data(data);

        let mut ff = FontFile {
            native: UnsafeCell::new(font_file),
            data_key: key,
            face_type: DWRITE_FONT_FACE_TYPE_UNKNOWN,
        };

        ff.analyze()
    }

    fn analyze(&mut self) -> u32 {
        let mut face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
        let mut num_faces = 0;
        unsafe {
            let mut supported = 0;
            let mut _file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;

            let hr = (*self.native.get()).Analyze(&mut supported,
                                                  &mut _file_type,
                                                  &mut face_type,
                                                  &mut num_faces);
            if hr != 0 || supported == 0 {
                return 0
            }
        }
        self.face_type = face_type;
        num_faces as u32
    }

    pub fn take(native: ComPtr<IDWriteFontFile>) -> FontFile {
        let mut ff = FontFile {
            native: UnsafeCell::new(native),
            data_key: 0,
            face_type: DWRITE_FONT_FACE_TYPE_UNKNOWN,
        };
        ff.analyze();
        ff
    }

    pub fn data_key(&self) -> Option<usize> {
        if self.data_key != 0 {
            Some(self.data_key)
        } else {
            None
        }
    }

    pub(crate) unsafe fn as_com_ptr(&self) -> ComPtr<IDWriteFontFile> {
        (*self.native.get()).clone()
    }

    // This is a helper to read the contents of this FontFile,
    // without requiring callers to deal with loaders, keys,
    // or streams.
    pub fn get_font_file_bytes(&self) -> Vec<u8> {
        unsafe {
            let mut ref_key: *const c_void = ptr::null();
            let mut ref_key_size: u32 = 0;
            let hr = (*self.native.get()).GetReferenceKey(&mut ref_key, &mut ref_key_size);
            assert!(hr == 0);

            let mut loader: ComPtr<IDWriteFontFileLoader> = ComPtr::new();
            let hr = (*self.native.get()).GetLoader(loader.getter_addrefs());
            assert!(hr == 0);

            let mut stream: ComPtr<IDWriteFontFileStream> = ComPtr::new();
            let hr = loader.CreateStreamFromKey(ref_key, ref_key_size, stream.getter_addrefs());
            assert!(hr == 0);

            let mut file_size: u64 = 0;
            let hr = stream.GetFileSize(&mut file_size);
            assert!(hr == 0);

            let mut fragment_start: *const c_void = ptr::null();
            let mut fragment_context: *mut c_void = ptr::null_mut();
            let hr = stream.ReadFileFragment(&mut fragment_start, 0, file_size, &mut fragment_context);
            assert!(hr == 0);

            let in_ptr = slice::from_raw_parts(fragment_start as *const u8, file_size as usize);
            let bytes = in_ptr.to_vec();

            stream.ReleaseFileFragment(fragment_context);

            bytes
        }
    }

    // This is a helper to get the path of a font file,
    // without requiring callers to deal with loaders.
    pub fn get_font_file_path(&self) -> Option<PathBuf> {
        unsafe {
            let mut ref_key: *const c_void = ptr::null();
            let mut ref_key_size: u32 = 0;
            let hr = (*self.native.get()).GetReferenceKey(&mut ref_key, &mut ref_key_size);
            assert!(hr == 0);

            let mut loader: ComPtr<IDWriteFontFileLoader> = ComPtr::new();
            let hr = (*self.native.get()).GetLoader(loader.getter_addrefs());
            assert!(hr == 0);

            let mut local_loader: ComPtr<IDWriteLocalFontFileLoader> =
                match loader.query_interface(&IDWriteLocalFontFileLoader::uuidof()) {
                    Some(local_loader) => local_loader,
                    None => return None,
                };

            let mut file_path_len = 0;
            let hr = local_loader.GetFilePathLengthFromKey(ref_key,
                                                           ref_key_size,
                                                           &mut file_path_len);
            assert_eq!(hr, 0);

            let mut file_path_buf = vec![0; file_path_len as usize + 1];
            let hr = local_loader.GetFilePathFromKey(ref_key,
                                                     ref_key_size,
                                                     file_path_buf.as_mut_ptr(),
                                                     file_path_len + 1);
            assert_eq!(hr, 0);

            if let Some(&0) = file_path_buf.last() {
                file_path_buf.pop();
            }

            Some(PathBuf::from(OsString::from_wide(&file_path_buf)))
        }
    }

    pub fn create_face(&self, face_index: u32, simulations: DWRITE_FONT_SIMULATIONS) -> FontFace {
        unsafe {
            let mut face: ComPtr<IDWriteFontFace> = ComPtr::new();
            let ptr = self.as_com_ptr();
            let hr = (*DWriteFactory()).CreateFontFace(self.face_type, 1, &ptr.as_ptr(),
                                                       face_index, simulations, face.getter_addrefs());
            assert!(hr == 0);
            FontFace::take(face)
        }
    }
}

impl Clone for FontFile {
    fn clone(&self) -> FontFile {
        unsafe {
            FontFile {
                native: UnsafeCell::new((*self.native.get()).clone()),
                data_key: self.data_key,
                face_type: self.face_type,
            }
        }
    }
}
