#pragma once

// =====================================================================
//  http/WebUi.h - 内嵌网页 UI
//
//  单页、零前端构建链：内嵌 HTML/JS，直接调用现有 JSON API
//  (/api/roots、/api/files、/api/download、/api/file、/api/mkdir)。
//  后期要换 Vue 时，只需把 / 换成静态资源托管，不动后端。
// =====================================================================

#include "http/Router.h"

#include <string>
#include <vector>

namespace nas {

void registerWebRoutes(Router& router, const std::vector<std::string>& roots,
                       const std::string& webDir = std::string());

}  // namespace nas
