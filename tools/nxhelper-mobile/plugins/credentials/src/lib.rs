use tauri::{
    plugin::{Builder, TauriPlugin},
    Manager, Runtime,
};

#[cfg(mobile)]
pub struct Credentials<R: Runtime>(tauri::plugin::PluginHandle<R>);
#[cfg(desktop)]
pub struct Credentials<R: Runtime>(std::marker::PhantomData<fn() -> R>);

impl<R: Runtime> Credentials<R> {
    pub fn call(
        &self,
        action: &str,
        value: serde_json::Value,
    ) -> Result<serde_json::Value, String> {
        #[cfg(mobile)]
        {
            self.0
                .run_mobile_plugin(action, value)
                .map_err(|e| e.to_string())
        }
        #[cfg(desktop)]
        {
            let _ = (action, value);
            Err("credentialStorageUnavailable".into())
        }
    }
}

pub fn init<R: Runtime>() -> TauriPlugin<R> {
    Builder::new("credentials")
        .setup(|app, _api| {
            #[cfg(target_os = "android")]
            let credentials = Credentials(
                _api.register_android_plugin("com.nxhelper.credentials", "CredentialsPlugin")?,
            );
            #[cfg(target_os = "ios")]
            let credentials = Credentials(_api.register_ios_plugin(init_plugin_credentials)?);
            #[cfg(desktop)]
            let credentials = Credentials::<R>(std::marker::PhantomData);
            app.manage(credentials);
            Ok(())
        })
        .build()
}

#[cfg(target_os = "ios")]
tauri::ios_plugin_binding!(init_plugin_credentials);
