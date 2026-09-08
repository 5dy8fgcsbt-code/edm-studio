# 随软件提供的第三方源码

## Eigen 3.4.0

`eigen-3.4.0/` 是本版本实际构建所用 Eigen 的完整、可编辑源码副本，共 1784 个文件、15,452,179 字节，与源码仓库内 `native/vendor/eigen/` 逐文件一致，未修改上游源码。本目录同时随 Windows 便携包和项目源码包提供，无需联网即可取得。

Eigen 的 MPL-2.0 覆盖文件依据 [COPYING.MPL2](eigen-3.4.0/COPYING.MPL2) 提供；各文件原有版权声明均保留。完整上游树还含 BSD、Apache、GPL、LGPL 等另有声明的附属代码、测试和示例，具体见各文件及全部 `COPYING.*`，不能把整个目录视作 MIT 或仅 MPL。程序使用 `Eigen/Dense`、`Eigen/Geometry` 和 `Eigen/SVD`，并定义 `EIGEN_MPL2_ONLY` 限制编译时引入非 MPL2 兼容模块；附属源码的随附不表示它们均被编入程序。

来源：

- [Eigen 官方 3.4.0 源码](https://gitlab.com/libeigen/eigen/-/tree/3.4.0)
- [官方版本 ZIP](https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.zip)
- 本次取得的官方 ZIP SHA256：`eba3f3d414d2f8cba2919c78ec6daab08fc71ba2ba4ae502b7e5d4d99fc02cda`
- 每个文件的 SHA256：`eigen-3.4.0.sha256.json`；该清单覆盖原始文件字节，不转换换行。

## 维护与验证

源码仓库中运行 `./native/verify-third-party-sources.ps1 -RequireBuildSource`，校验上述副本与实际构建目录。`native/package.ps1` 在打包前检查两个目录，并逐文件检查生成的 Windows ZIP 和源码 ZIP；缺文件或内容不匹配会终止打包。

升级或修改 Eigen 时，应同步更新版本目录、完整源码、来源和校验清单，并记录修改。Eigen 自带的 `.gitignore` 在 Windows 上会忽略 `Core` 等源码；首次纳入或更新时使用 `git add -f native/vendor/eigen third_party_sources/eigen-3.4.0`，再检查 Git 归档。根目录 `.gitattributes` 保留这两个目录的原始字节。

项目原创代码采用根目录 [LICENSE](../LICENSE) 中的 MIT；此授权不替换 Eigen 或其他第三方的许可证。其他组件、字体与 EDM 解析来源见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
