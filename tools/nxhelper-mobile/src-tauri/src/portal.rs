use crate::protocol::private_ipv4;
use reqwest::{Client, Method, Response, Url};
use rustls::{
    client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier},
    pki_types::{CertificateDer, ServerName, UnixTime},
    DigitallySignedStruct, SignatureScheme,
};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{sync::Arc, time::Duration};

pub type Result<T> = std::result::Result<T, String>;
#[derive(Clone, Deserialize, Serialize)]
pub struct Credentials {
    pub user: String,
    pub password: String,
}

// Only installed on a client whose single origin is a validated private IPv4:11443.
// The TLS handshake signature is still checked: only chain/name trust is relaxed.
#[derive(Debug)]
struct XboxVerifier;
impl ServerCertVerifier for XboxVerifier {
    fn verify_server_cert(
        &self,
        _: &CertificateDer<'_>,
        _: &[CertificateDer<'_>],
        name: &ServerName<'_>,
        _: &[u8],
        _: UnixTime,
    ) -> std::result::Result<ServerCertVerified, rustls::Error> {
        match name {
            ServerName::IpAddress(ip) if private_ipv4(&std::net::IpAddr::from(*ip).to_string()) => {
                Ok(ServerCertVerified::assertion())
            }
            _ => Err(rustls::Error::General("Private Xbox IPv4 required".into())),
        }
    }
    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &rustls::crypto::ring::default_provider().signature_verification_algorithms,
        )
    }
    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &rustls::crypto::ring::default_provider().signature_verification_algorithms,
        )
    }
    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        rustls::crypto::ring::default_provider()
            .signature_verification_algorithms
            .supported_schemes()
    }
}

pub struct Portal {
    client: Client,
    origin: Url,
    credentials: Credentials,
    csrf: String,
}
impl Portal {
    pub fn new(host: &str, credentials: Credentials) -> Result<Self> {
        if !private_ipv4(host) {
            return Err("privateIpRequired".into());
        }
        let tls = rustls::ClientConfig::builder_with_provider(Arc::new(
            rustls::crypto::ring::default_provider(),
        ))
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(XboxVerifier))
        .with_no_client_auth();
        let client = Client::builder()
            .use_preconfigured_tls(tls)
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .connect_timeout(Duration::from_secs(10))
            .timeout(Duration::from_secs(12 * 3600))
            .build()
            .map_err(|e| e.to_string())?;
        Ok(Self {
            client,
            origin: Url::parse(&format!("https://{host}:11443")).map_err(|e| e.to_string())?,
            credentials,
            csrf: String::new(),
        })
    }
    pub async fn request(
        &mut self,
        method: Method,
        endpoint: &str,
        query: &[(String, String)],
        form: Option<reqwest::multipart::Form>,
    ) -> Result<Response> {
        if method != Method::GET && self.csrf.is_empty() {
            self.raw(Method::GET, "/api/os/machinename", &[], None)
                .await?;
        }
        self.raw(method, endpoint, query, form).await
    }
    async fn raw(
        &mut self,
        method: Method,
        endpoint: &str,
        query: &[(String, String)],
        form: Option<reqwest::multipart::Form>,
    ) -> Result<Response> {
        // Endpoints are fixed by this module; never accept another origin or redirects.
        let mut url = self.origin.clone();
        url.set_path(endpoint);
        let mut request = self.client.request(method, url).query(query);
        if !self.credentials.user.is_empty() {
            request = request.basic_auth(&self.credentials.user, Some(&self.credentials.password));
        }
        if !self.csrf.is_empty() {
            request = request
                .header("Cookie", format!("CSRF-Token={}", self.csrf))
                .header("X-CSRF-Token", &self.csrf);
        }
        if let Some(form) = form {
            request = request.multipart(form);
        } else {
            request = request.timeout(Duration::from_secs(60));
        }
        let response = request
            .send()
            .await
            .map_err(|_| "connectionFailed".to_string())?;
        for cookie in response.headers().get_all("set-cookie") {
            if let Ok(cookie) = cookie.to_str() {
                for part in cookie.split(';') {
                    if let Some(token) = part.trim().strip_prefix("CSRF-Token=") {
                        self.csrf = token.to_string();
                    }
                }
            }
        }
        if !response.status().is_success() {
            return Err(format!("portalHttp:{}", response.status().as_u16()));
        }
        Ok(response)
    }
    pub async fn json(&mut self, endpoint: &str, query: &[(String, String)]) -> Result<Value> {
        self.request(Method::GET, endpoint, query, None)
            .await?
            .json()
            .await
            .map_err(|_| "portalInvalidResponse".into())
    }
    pub async fn packages(&mut self) -> Result<Vec<String>> {
        let data = self.json("/api/app/packagemanager/packages", &[]).await?;
        Ok(data["InstalledPackages"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|p| p["PackageFullName"].as_str().map(String::from))
            .collect())
    }
    pub async fn free_space(&mut self) -> Option<u64> {
        let data = self.json("/api/filesystem/drives", &[]).await.ok()?;
        let drive = data["Drives"].as_array()?.iter().find(|d| {
            d["Name"].as_str().is_some_and(|n| {
                n.eq_ignore_ascii_case("C:") || n.to_ascii_lowercase().starts_with("c:\\")
            })
        })?;
        drive["FreeSpaceInBytes"]
            .as_u64()
            .or_else(|| drive["FreeSpace"].as_u64())
    }
    pub fn query(package: &str, directory: &str) -> Vec<(String, String)> {
        vec![
            ("knownfolderid".into(), "LocalAppData".into()),
            ("packagefullname".into(), package.into()),
            (
                "path".into(),
                if directory.is_empty() {
                    String::new()
                } else {
                    format!("\\{}", directory.replace('/', "\\"))
                },
            ),
        ]
    }
    pub async fn folder(&mut self, package: &str, parent: &str, name: &str) -> Result<()> {
        let mut query = Self::query(package, parent);
        query.push(("newfoldername".into(), name.into()));
        if let Err(error) = self
            .request(Method::POST, "/api/filesystem/apps/folder", &query, None)
            .await
        {
            let data = self
                .json("/api/filesystem/apps/files", &Self::query(package, parent))
                .await?;
            if !data["Items"]
                .as_array()
                .is_some_and(|items| items.iter().any(|i| i["Name"].as_str() == Some(name)))
            {
                return Err(error);
            }
        }
        Ok(())
    }
    pub async fn upload(
        &mut self,
        package: &str,
        directory: &str,
        name: &str,
        size: u64,
        body: reqwest::Body,
    ) -> Result<()> {
        let mut query = Self::query(package, directory);
        query.push(("extract".into(), "false".into()));
        let part = reqwest::multipart::Part::stream_with_length(body, size)
            .file_name(name.to_string())
            .mime_str("application/octet-stream")
            .map_err(|e| e.to_string())?;
        self.request(
            Method::POST,
            "/api/filesystem/apps/file",
            &query,
            Some(reqwest::multipart::Form::new().part("file", part)),
        )
        .await?;
        let data = self
            .json(
                "/api/filesystem/apps/files",
                &Self::query(package, directory),
            )
            .await?;
        if !data["Items"].as_array().is_some_and(|items| {
            items.iter().any(|item| {
                item["Name"].as_str() == Some(name)
                    && item["FileSize"]
                        .as_u64()
                        .or_else(|| item["SizeInBytes"].as_u64())
                        == Some(size)
            })
        }) {
            return Err("uploadSizeMismatch".into());
        }
        Ok(())
    }
}
