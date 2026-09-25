# 稳定更新入口

客户端固定读取本目录的 `stable.json`。该文件是 P-256 签名封装，必须由 `tools/UpdatePublisher` 生成。

该文件同时镜像到 Gitee（`https://gitee.com/tan-xuedong/imao-updates/raw/main/stable.json`），因为 `raw.githubusercontent.com` 在国内无代理时不可达，更新检查曾经随之整体失败。镜像只提供可达性、不提供权威性：客户端按顺序尝试本目录的地址与镜像，任何一份都必须通过同一个固定公钥验签才会被使用，因此镜像最多只能"拒绝服务"。镜像地址写在 `UpdateService.ManifestMirrors` 与 `scripts/Set-GiteeMirror.ps1` 两处，`Tests/ResourceUpdates` 会断言两者一致。

首次正式发布之前不创建占位清单。程序启动检查遇到尚不存在的入口时报告检查失败，手动安装的内置资源仍可使用。

通过 `scripts/Publish-ResourceUpdate.ps1` 完成草稿上传、内容核对、正式发布及公开下载验证之后，最后一步才会创建或更新 `stable.json`。Gitee 镜像在这一步之后推送，且失败只告警、不使发布失败——镜像永远不会领先于权威渠道。不要手工编辑签名内容，不要提交未签名示例或测试密钥签名清单到稳定入口。清单序号必须递增；相同序号不得对应不同内容。

Gitee 推送由 `scripts/Set-GiteeMirror.ps1` 完成，需要 `%LOCALAPPDATA%\WWMAP-TOOLS-Publisher\gitee-token.txt`（仓库外，与发布私钥同目录）。该脚本可以独立运行以核对镜像链路：`scripts/Set-GiteeMirror.ps1 -Manifest updates/stable.json`。没有令牌文件时脚本报错，而 `Publish-ResourceUpdate.ps1` 把它降级为告警并继续。

维护步骤见 [资源更新说明](../Docs/ResourceUpdates.md)。
