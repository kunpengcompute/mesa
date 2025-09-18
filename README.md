# 项目介绍<a name="ZH-CN_TOPIC_0000002475600541"></a>

mesa代码仓是鲲鹏使能云手机图形渲染的能力的demo参考。它基于mesa开源社区版本，集成了针对鲲鹏云手机平台的GPU驱动适配以及渲染性能优化的补丁。

# 版本说明<a name="ZH-CN_TOPIC_0000002442160584"></a>

<a name="table740710612324"></a>
<table><thead align="left"><tr id="row144075603214"><th class="cellrowborder" valign="top" width="31.069999999999997%" id="mcps1.1.3.1.1"><p id="p114078693216"><a name="p114078693216"></a><a name="p114078693216"></a>版本</p>
</th>
<th class="cellrowborder" valign="top" width="68.93%" id="mcps1.1.3.1.2"><p id="p19408196163219"><a name="p19408196163219"></a><a name="p19408196163219"></a>说明</p>
</th>
</tr>
</thead>
<tbody><tr id="row64089653211"><td class="cellrowborder" valign="top" width="31.069999999999997%" headers="mcps1.1.3.1.1 "><p id="p18715191219115"><a name="p18715191219115"></a><a name="p18715191219115"></a>22.1.7</p>
</td>
<td class="cellrowborder" valign="top" width="68.93%" headers="mcps1.1.3.1.2 "><p id="p19172114781912"><a name="p19172114781912"></a><a name="p19172114781912"></a>安卓11的图形驱动层</p>
</td>
</tbody>
</table>

# 环境部署<a name="ZH-CN_TOPIC_0000002475680361"></a>

完整构建指南请查阅文档[docs/install.rst](https://mesa3d.org/meson.html)，推荐使用Meson构建，Meson版本要求如下：

-   22.1.7版本：Meson \>= 0.63.2

使用Meson的构建方法如下：

```
mkdir build
cd build
meson ..
sudo ninja install
```

# 贡献指南<a name="ZH-CN_TOPIC_0000002442160592"></a>

如果使用过程中有任何问题，或者需要反馈特性需求和bug报告，可以提交issue联系我们，具体贡献方法可参考[这里](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md)。

# 免责声明<a name="ZH-CN_TOPIC_0000002442320452"></a>

此代码仓仅包含功能演示与开发示例代码，旨在展示特定功能的使用方式与集成方法，不用于生产环境。所有代码仅为技术参考，不继承或承诺任何上下游软件的安全设计与防护机制。本仓库中的示例代码可能存在安全缺陷、漏洞或不完整实现，鲲鹏计算社区不对代码的安全性、稳定性及合规性承担任何责任。使用者应自行评估风险，并根据实际场景进行安全加固。任何因使用本仓库代码所引发的安全问题，均由使用者自行承担。请勿将本仓库代码直接用于生产系统，建议持续关注上游开源项目的安全公告与版本更新。。