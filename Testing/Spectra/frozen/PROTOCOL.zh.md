# NDI RAW over OpenIGTLink v1.0 — 冻结实施合同

状态：2026-10-27，供 Plus 发送端与 RMT 接收端共同实现的设计基线；尚非已实现/已部署功能。只在本独立协议目录交付，不修改源码、驱动、生产配置或既有录制。`SCHEMA.json` 为字段/类型合同，本文件的跨字段、生命周期及错误规则同样具有约束力。任何 wire 字段/语义改动须同时通知两端并更新版本和 golden，不得各自悄改。

## 1. 传输、命名与版本

- 保留原 TRANSFORM 端口/消息/状态语义。v1 raw 推荐独立、显式启用的 IGTL TCP server endpoint，默认建议 **18945**；现有 pose 常用18944不变。端口可配置且不得占用时静默换端口。暂不混入现有按 tracked-frame 时间采样的 Plus server 队列；若今后共用连接，须仍满足本合同逐 BX 顺序和单 socket writer，不能并发交叉写消息。
- 标准 OpenIGTLink 消息：`TYPE=STRING`，`DEVICE_NAME=NDI_RAW_V1`，**header version=1**（这是标准头版本，不是应用协议版本），header timestamp=0。STRING 内 `ENCODING=106`（UTF-8），LENGTH 为 UTF-8 字节数；标准 IGTL body CRC64 必须填写并校验。不得冒充 TRANSFORM 或修改其标准正文。这里选择标准 STRING 的应用层 JSON；custom binary 可避免JSON膨胀，但另需双方实现/协商，v1不采用。
- STRING 的 UTF-8 payload **1..65535 bytes**；正文为4字节STRING头+payload。单条一个JSON对象，无BOM/NUL/尾随非空白数据。不得按任意字节截断JSON，不做应用分片。单帧过大→保留本地snapshot/sidecar，记 `OVERSIZE_FRAME`、消耗对应seq并计未入队drop或入队后discard（只计一项），本raw session不完整；不能丢字段后谎称完整。v1 FULL_REQUIRED保证已发送frame含完整raw wire；要放宽须新版本。
- payload固定 `schema="ndi.raw.v1"`，`schema_minor=0`。v1采用**显式配置版本 + 首条session能力声明/接收校验**，不实现hello往返/动态降级，raw端口不接受远程采集/ROM/serial控制，任何网络输入不得转发NDI；这不是协商ACK。接收端只启用其明确支持的v1.0。首条必须session，检查source_id、版本、约定和长度；不支持即关闭/隔离raw连接并提示，原TRANSFORM与RMT科学采集继续。将来不兼容major使用新schema和设备名；minor变更亦须两端同步。`extensions` 是唯一扩展命名空间，接收端可保留未知扩展而不得据其标有效。
- JSON禁止重复key、NaN/Infinity、非有限数、超范围整数、过深结构（最大16层）及类型强制转换。所有uint64、纳秒、epoch、seq、最终计数用十进制字符串（无符号、无前导零、值≤18446744073709551615）；device uint32 counter和小状态码用JSON整数。XYZ/pose用有限JSON number，序列化至少保留原float32准确值（推荐double往返17位，不虚称提高精度）。

## 2. 身份与记录生命周期

公共字段见schema：`schema/schema_minor/record_type/source_id/acquisition_id/session_id/seq/extensions/data`。

- `source_id`：配置中稳定的源UUID；不是IP、handle或物理工具认证。一个raw endpoint只发一个source，接收端显式绑定，变化必须新session。
- `acquisition_id`：一次连续NDI采集上下文UUID。进程/采集重启、NDI重连或完成失同步恢复后生成新的ID。单纯TCP重连保持它；这不证明设备物理真实性。
- `session_id`：一次raw网络流上下文UUID。每次TCP连接/重新订阅，以及acquisition变化时重新生成。一个TCP连接可按`end→session`顺序承载新逻辑session；没有end则前session完整性未知/失败，不能被新session修复。
- `seq`：本session所有记录的严格递增序号，session=0。**在尝试入网络队列前分配**，失败也消耗；接收端因此可见缺口。无历史重放v1：TCP重连先新session，再发未来snapshot，不能以时间戳拼接旧流。`previous_session_id`可关联历史但不补缺口。
- `reply_seq`：acquisition内每次现有BX调用的主机尝试序号，从1开始，在调用前分配，包括超时/CRC/设备错误。每次调用完成产生且只产生一个不可变snapshot，对应一条`frame`（错误回复也用frame）；源端不增加poll。`frame_id=acquisition_id+":"+reply_seq`。这不是设备帧号或曝光时戳。
- `session.data.first_reply_seq`是producer上建立的原子开始边界（该边界之后第一个BX尝试）；session先入控制队列，随后覆盖的snapshot才可入帧队列。结束在同样明确的producer边界冻结`last_reply_seq`，后续poll可继续供TRANSFORM但不属于本session。不能用网络到达时刻反推边界。
- 记录：`session`给能力、时钟域、初始绑定、来源；`frame`含一次BX成功或失败；`error`报告协议/导出/传输异常，不替代本应存在的BX frame；`end`给该流收尾计数/摘要。正常设备返回错误但完整保存在frame中，不自动等于存储不完整。无end始终不能宣称该流完整。

## 3. 原始观测、工具与slot

- `frame.data.reply`含请求flags、本次valid/error/CRC、完整raw_reply_hex、字节数和SHA256（包含所收完整或残缺字节，空串SHA有定义）、system status。`timing`对应同次调用。CRC检查合格不是请求新鲜认证；同步恢复由recovery_state另表达。
- `tools`包含本次配置绑定的工具（即使未报告，reported=false也保留明确缺失条目）及本次未绑定的handle。重复handle/tool_id等冲突拒绝该frame作有效测量，保留wire诊断。未绑定项tool_id/body_frame等可null，不能自动当另一工具。
- 每工具携带当前handle、tool_id、transform/body frame、ROM字节SHA256/MD5/长度（未知null）、binding_epoch。ROM哈希不是物理实体认证，也不是marker几何解析。handle重用、归属/ROM/定义变化递增binding_epoch，清新帧状态；每frame自包含绑定，不能靠上一frame补未知项。
- 当前BX0002提供A..T二十状态槽：`marker_slots`是**动态列表**，最多20项，不固定4球、也不要求A–D齐全。slot_index=0..19、slot_label=A+index一一相符且唯一。additional_info_available=true时必须保存全部20状态槽（包括status0、无坐标及invalid槽）；false时marker_slots为空。marker_count_reported保留0008 compact数量，无报告null；marker_identity_known保留本次映射结论。status=0仅表示missing或未定义，不能猜ROM成员。
- `defined_slots`仅在已解析ROM成员或明确配置时列出，source标明ROM_DECODED/CONFIGURATION；否则null/UNAVAILABLE。GUI可显示“有效坐标3，定义球数未知”；不得因为见过4球就推断分母4。4/4只能来自已知defined_slots，不能由工具pose有效推断。
- identity mapping按本次0002状态槽与0008 compact点一致性；已知slot坐标才能放marker_slots。未知对应的实测点全部放`unidentified_positions`（reply_local_index只在本回复有意义），具名slot的xyz为null/identity_known=false；禁止按数组顺序猜A–D或跨帧补点。不得零向量/上一valid填未知。
- xyz_mm在NDI position-sensor/reference_frame（例如Tracker）中，右手XYZ、mm，直接来自0008 wire float32；不是ROM局部坐标，不能pose×ROM反算。`visibility`只描述本次坐标/OOV，不等于参与pose fit。原始device_status保留0..15，不用人类标签覆盖。
- marker.sample_valid精确定义：本回复结构/CRC valid、frame与tool的recovery_state均SYNCHRONIZED、工具本次reported、slot身份确定且本次有限XYZ存在、OOV=false、device_status==3。它**不合取pose有效、4球齐全或fresh**。XYZ非有限→全triplet null、finite=false、sample_valid=false，raw wire仍保留。status4等其它原始状态保留坐标但sample_valid=false。
- `raw_pose`只从同BX同handle块pose_present读取；缺失null。quaternion_wxyz+translation_mm表示原ROM工具局部→reference的主动旋转/平移，p_ref=R(q)p_tool+t；不应用ToolToTip/额外世界配准。fit_error_raw原样保存设备报告值，fit_error_unit仅在目标设备/固件协议资料已核实时填mm并以fit_error_basis记依据，否则UNKNOWN；不沿袭旧sidecar的_mm字段名就假定单位。它不是本任务精度。raw四元数不默默归一化；本v1数值结构检查固定 abs(sum(q_i*q_i)-1)<=1e-3，且全有限，超出即raw_pose.sample_valid=false并记QUATERNION_NOT_UNIT。这是新raw合同的数值检查，不改变既有TRANSFORM/science门槛。派生显示矩阵若归一化副本须标明，不能改raw值。错误status不能标有效。有pose但某分量非有限时仍保留raw_pose对象，finite=false；该q/t整个数组或fit_error_raw置null并记原因，不能置零或消失成未报告。
- raw_pose.sample_valid要求本次reply valid、frame与tool的recovery_state均SYNCHRONIZED、reported、pose_present、所有数值有限及可用刚性quaternion、**现有driver从本次BX状态得到tool_status=OK**。tool_status_origin必须PLUS_CURRENT_BX_STATUS，未知时UNKNOWN并invalid。实现者须沿用并记录当前工具状态mask逻辑，不从旧矩阵/ToolTimeStampedUpdate返回码推有效。
- `plus_submission`仅可装旧Plus提交参数/矩阵，并固定semantics=SUBMISSION_SNAPSHOT_NOT_BUFFER_OR_NETWORK_RECEIPT；该矩阵可能在无效状态时是legacy旧值，不能赋给raw_pose或用于具名球。既有TRANSFORM可能经滤时、插值、重复，不能假设它与raw bundle逐帧相同。raw pose与marker同BX同工具块，但无独立marker曝光时间，不能声称同曝光。

## 4. counter、新鲜度与隔离

- 每工具保留uint32原值，按(acquisition_id,connection_epoch,binding_epoch,tool_id/handle)分类；`counter.epoch`维护counter不连续域。第一可见值FIRST/new=null；同值REPEAT/new=false；unsigned delta在(0,2^31)且counter不回退为NEW/new=true。设备delta大于1不是网络/写盘丢帧证据。
- counter回退且modulo delta<2^31为WRAP_CANDIDATE；其它反向/大跳为RESET_OR_OUT_OF_ORDER。v1无独立证据能可靠区分wrap/reset，均递增counter.epoch并将tool.recovery_state设为QUARANTINED，**隔离**本条及该工具后续有效性，不把猜测wrap直接当新有效帧；is_new=null。同counter而raw工具块改变也隔离并报错。其它工具可保留自身状态，但全串口timeout/半帧等失同步隔离整个reply。
- 隔离状态rawXYZ仍保存但sample_valid=false。单纯下一counter递增、CRC成功、flush或epoch+1不足恢复；经本地已定义/验证的串口/协议恢复流程后新acquisition/session，首次counter重新FIRST。无事务ID时保留关联不确定性，不能用counter伪造请求认证。
- RMT本地另以接收monotonic和显式UI stale_timeout判断“最近收到”；该TTL是显示可用性策略，不是源曝光年龄证明，不修改科学有效性。FIRST可作首次当前观测但不声称已证新设备帧；REPEAT不累加唯一帧率，陈旧/隔离状态不得继续显示绿色新鲜valid。分别显示pose状态、每球sample_valid、counter状态和接收stale。
- 独立设备帧key=(source_id,acquisition_id,connection_epoch,binding_epoch,tool_id或handle,counter.epoch,counter.value)；reply_seq与IGTL seq不代替此key。

## 5. 时钟

所有时间来自原snapshot；不得封包时覆盖成“测量时刻”。command_start/receive/decode_done是源host CLOCK_MONOTONIC ns，receive_wall是CLOCK_REALTIME UNIX ns；失败/不可用用null，round_trip为receive-command的非负差（不存在/倒退null并报诊断）。clock_domain_id只在同process/boot已确认连续域内可比较；源重启新ID。接收端保存自己的host/clock_domain和接收monotonic/UTC，跨机monotonic不能相减。

session明示cross_host_clock_relation=UNSYNCHRONIZED，device_measurement_timestamp固定null。源UTC和monotonic成对采样不等于跨机同步或曝光时间；wallclock跳变须诊断。未来PTP/offset估计必须独立版本化含方法/误差，不能伪填当前字段。IGTL timestamp=0避免再造一个混淆时基。

## 6. SSOT、背压与收尾

- 在采集线程建立一个有界成本、不可变、只含本次BX+绑定/计数状态的**内存snapshot SSOT**，在分发前完成身份/counter语义，network和sidecar各自引用/复制它。主生产链不能先落sidecar再反向解析文件；不得第二串口reader/额外BX poll。文件/JSON编码/socket/日志I/O在worker；producer不等待网络或磁盘。预分配/有界队列/try策略及失败计数需可测，不能承诺任意负载无限lossless。
- v1 network队列有明确条数/字节上限，超限DROP_NEW；对应seq/reply_seq已消耗，frames_dropped++，本session sender_complete永久false。错误状态sticky，日志限频但状态/计数不得丢。控制消息另留有界保留容量，但单writer严格按seq顺序发送，不能优先越过已排队早期帧；错误消息也无法发送时，本地状态/summary仍必须失败，接收端无end只能判未知。部分socket写/超时关闭该TCP流，不能接着把残余消息当新头。
- source停止/导出停止先在producer冻结边界，再worker有界排空；所有已分配frame须先发出或记为discard，end不得越过它们；超时剩余计frames_discarded，最终发送end若可行，随后关闭。反复Stop不得把失败翻成true。正常工具TRANSFORM可按既有设计继续，raw失败必须GUI/状态明显可见。
- per-session最终frames计数（全部十进制字符串）：considered=该边界内BX snapshots数量；enqueued=成功入raw队列；dropped=未入队（队列/分配失败）；sent=完整STRING发送调用完成；discarded=已入队但编码/尺寸失败、发送失败/关闭/超时未完整送出。final必须 considered=enqueued+dropped、enqueued=sent+discarded，且连续reply区间长度=considered（零帧last_reply_seq=null）。发送成功不是对端收到/解析/落盘确认。
- `sent_frame_payload_sha256`：按实际完整发送成功顺序，对每条frame的**原始UTF8 JSON payload字节**串联 `uint32 big-endian长度 || payload` 后SHA256（不含IGTL/STRING头、不含session/error/end）。零帧为空SHA256。不要重新JSON编码后算接收摘要。接收端边收/校验边用同字节做digest，不能只比较最后一帧。
- end.seq也必须无缺口；end中的sender_complete只在零drop/discard/transport或control丢失、计数守恒、成功终结时true。设备无效观测/超时若如实完整保存可以存储完整，但测量仍invalid（golden即此负例）。error.affects_completeness=true永久使sender_complete=false。
- RMT必须独立保存wire/payload、解析状态、接收时钟、seq/reply缺口、重复/冲突、session来源、拒绝原因、最终摘要。接收端storage_complete=true需：支持的session；全部seq/frame区间/计数/digest一致；可信完整end；自身未丢/拒/截断；文件flush/fsync/close及summary发布成功。没有end/摘要或writer失败都不能绿色完整。完整区间之前/之后未订阅历史不作完整声明。若要宣称断电目录项耐久，还需另做目录fsync合同，v1不声称已具备。
- 对无效schema、CRC、超长/截断、重复session/seq冲突严格拒绝相应raw流，不伪造观测；正确framing的未知类型可以有界跳过并诊断，但不能算合格raw数据。raw故障不能主动停止RMT RAW或改变其科学算法/阈值。

## 7. sidecar兼容、GUI和验收

现有sidecar过渡保留。共同frame_id/reply_seq关联同snapshot，逐字段核对wirehex/hash、状态、槽列表、坐标、原pose、绑定、时钟、Plus提交诊断等，不得仅比行数。旧字段名/JSON数值→dec-string转换可显式映射，不能丢语义。现有计数是每sink并发快照，不等于当前行耐久；network/sidecar各报自身最终sink计数。现有sidecar旧schema默认不变；新增frame关联字段需记录兼容扩展，不能冒称旧软件已支持。

GUI至少：raw连接/版本、source/acquisition/session、pose状态、每tool有效球数及已知/未知分母、slot状态/XYZ、counter/fresh/stale、raw错误/丢失/记录完整性。工具pose OK不推出marker 4/4；slot状态不替代tool级输出。显示降频与保存全量分开。

必须测试：schema golden + 真实捕获字节fixture（标来源）；0球/部分球/未知身份/非有限/status4/OOV/有效pose但缺球；framecounter重复、payload变化、回退/wrapcandidate；CRC/timeout/迟到A→B隔离；TCP逐字节/任意分片/多消息粘包/短写/断连；UTF8长度65535边界/oversize/重复key/NaN/深度；seq缺口/重复/乱序/跨session注入；重连无历史重放；队列饱和/Stop两次/所有write-fsync-close失败；摘要和接收文件耐久状态分离。线下通过后本机Spectra同BX20Hz持续录制并与sidecar逐frame对账，报告时长/计数/最大积压/故障结果；不承诺永远无丢失。

`GOLDEN.jsonl`是合成3记录生命周期：一次空回复timeout、QUARANTINED、没有有效工具/球，仍可sender存储完整。它不冒充真实CRC/wire。`ERROR-SHAPE.json`仅error字段形状，非完整有序流。Plus agent需另补真实正常/多slot/错误字节golden供RMT共用；现有schema测试不能代替串口故障测试。

## 8. 实现边界与待核实

默认不启用/不部署新endpoint，不改变现录制配置、曝光、波特率、NDICAPI_SERIAL_WRITE_DELAY_US、分片读取修复或科学QC。Plus开发在用户本机进行；RMT由其唯一实现线程改接收器/GUI。本目录只交付合同。若当前设备的坐标轴/pose convention不能由本机源码/厂商协议确认，应在首条session拒绝启用该profile，不猜约定；原工具状态mask/二进制绑定、rawfield到旧sidecar映射及串口恢复流程由本机agent明确验证并回报。协议定义不是验证结果。
