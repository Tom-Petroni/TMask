unsafe extern "C" {
    fn tmask_base_keepalive();
}

#[unsafe(no_mangle)]
pub extern "C" fn tmask_base_rust_link() {
    unsafe {
        tmask_base_keepalive();
    }
}

