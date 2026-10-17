// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside Drive: both providers share the same reconciliation engine.
private:
std::string DavChild(const std::string& parent, const std::string& name) {
    ValidateCloudName(name);
    return (parent.empty() ? config_.dav_url : parent) + "/" + UrlEncode(name);
}
std::optional<std::string> DavFind(const std::string& parent, const std::string& name) {
    const auto path = DavChild(parent, name);
    HttpRequest request;
    request.method = "PROPFIND";
    request.url = path;
    request.headers["Depth"] = "0";
    const auto reply = Call(std::move(request), true, "WebDAV find folder");
    if (reply.status == 404)
        return std::nullopt;
    if (reply.status != 207)
        Fail("WebDAV find folder", reply);
    winrt::Windows::Data::Xml::Dom::XmlDocument xml;
    xml.LoadXml(winrt::to_hstring(reply.Text()));
    if (xml.SelectNodes(L"//*[local-name()='collection']").Size() == 0)
        throw SyncError("WebDAV path is not a folder");
    return path;
}
std::string DavCreate(const std::string& parent, const std::string& name) {
    const auto path = DavChild(parent, name);
    HttpRequest request;
    request.method = "MKCOL";
    request.url = path;
    const auto reply = Call(std::move(request), false, "WebDAV create folder");
    if (reply.status != 201)
        Fail("WebDAV create folder", reply);
    return path;
}
std::vector<CloudItem> DavList(const std::string& folder) {
    HttpRequest request;
    request.method = "PROPFIND";
    request.url = folder;
    request.headers["Depth"] = "1";
    request.content_type = "application/xml";
    request.body = ToBytes("<d:propfind "
                           "xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/"
                           "><d:getlastmodified/></d:prop></d:propfind>");
    const auto reply = Call(std::move(request), true, "WebDAV list folder");
    if (reply.status != 207)
        Fail("WebDAV list folder", reply);
    winrt::Windows::Data::Xml::Dom::XmlDocument xml;
    xml.LoadXml(winrt::to_hstring(reply.Text()));
    std::vector<CloudItem> children;
    std::string parent = winrt::to_string(wf::Uri{winrt::to_hstring(folder)}.Path());
    while (parent.ends_with("/"))
        parent.pop_back();
    for (const auto& node : xml.SelectNodes(L"//*[local-name()='response']")) {
        const auto href = node.SelectSingleNode(L"*[local-name()='href']");
        if (!href)
            throw SyncError("WebDAV response missing href");
        std::string path = winrt::to_string(href.InnerText());
        if (path.rfind("https://", 0) == 0)
            path = winrt::to_string(wf::Uri{winrt::to_hstring(path)}.Path());
        while (path.ends_with("/"))
            path.pop_back();
        if (path == parent)
            continue;
        if (!path.starts_with(parent + "/"))
            throw SyncError("WebDAV child outside folder");
        const std::string relative = path.substr(parent.size() + 1);
        if (relative.find('/') != std::string::npos)
            throw SyncError("WebDAV depth mismatch");
        CloudItem item;
        item.name = winrt::to_string(wf::Uri::UnescapeComponent(winrt::to_hstring(relative)));
        ValidateCloudName(item.name);
        item.id = DavChild(folder, item.name);
        const auto prop = node.SelectSingleNode(L"*[local-name()='propstat'][contains(*[local-name("
                                                L")='status'],' 200 ')]/*[local-name()='prop']");
        if (!prop)
            throw SyncError("WebDAV properties unavailable");
        item.is_folder =
            prop.SelectNodes(L"*[local-name()='resourcetype']/*[local-name()='collection']")
                .Size() != 0;
        const auto size = prop.SelectSingleNode(L"*[local-name()='getcontentlength']");
        if (size) {
            item.has_size = true;
            item.size = std::stoull(winrt::to_string(size.InnerText()));
        }
        children.push_back(item);
    }
    return children;
}

public:
