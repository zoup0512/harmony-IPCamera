import { MailSettings, FtpSettings, endpoint, mail, ftp, safeFailure, editDestination, editCredential, credentialsBound, authTarget } from '../util/DeliveryProtocol';
import { loadMail, loadFtp, saveMail, saveFtp } from '../util/DeliveryStore';
import { PlatformDialer } from '../util/DeliverySocket';
import { lastDeliveryResult } from '../util/DeliveryService';
import { ANDROID_ACCENT, ANDROID_BG, ANDROID_DIVIDER, ANDROID_SECONDARY, ANDROID_TEXT } from '../components/AndroidTheme';
import { SettingsSection, SettingsToolbar, SettingsWarning, settingsAction, settingsField } from '../components/ExtensionSettings';

// Only interpolate the numeric reply code, never server text, credentials or paths.
function deliveryMessage(reason: string): Resource {
  switch (reason) {
    case '无法读取安全存储，请解锁设备后重试': return $r('app.string.addon_delivery_vault_read');
    case '配置超过安全存储的 1024 字节上限': return $r('app.string.addon_delivery_vault_size');
    case '无法保存至安全存储，请解锁设备后重试': return $r('app.string.addon_delivery_vault_save');
    case 'SMTP 未开启': return $r('app.string.addon_delivery_mail_off');
    case '请填写有效的 SMTP 主机、端口、发件及收件地址': return $r('app.string.addon_delivery_mail_fields');
    case '请重新填写当前 SMTP 服务器的用户名和密码': return $r('app.string.addon_delivery_mail_auth');
    case 'FTP 未开启': return $r('app.string.addon_delivery_ftp_off');
    case '请先确认对该服务器使用明文 FTP': return $r('app.string.addon_delivery_consent_required');
    case '请检查 FTP 用户名和密码': return $r('app.string.addon_delivery_ftp_auth');
    case '目录仅支持字母、数字、横线、下划线及斜线': return $r('app.string.addon_delivery_ftp_directory');
    case '证书 SAN 与 SMTP 主机不符': return $r('app.string.addon_delivery_cert');
    case 'SMTP TLS 连接或证书验证失败，请检查主机名、证书和隐式 TLS 端口': return $r('app.string.addon_delivery_tls');
    case 'FTP 连接失败，请检查主机、端口和网络': return $r('app.string.addon_delivery_connection');
    case '等待服务器响应超时': return $r('app.string.addon_delivery_timeout');
    case '服务器未提供 PLAIN 或 LOGIN 认证': return $r('app.string.addon_delivery_auth_method');
    case '连接已中断': return $r('app.string.addon_delivery_interrupted');
    case '配置已更改，已中止传输': return $r('app.string.addon_delivery_changed');
    case '录像仍在写入，未上传': return $r('app.string.addon_delivery_in_progress');
    case '文件已更改，已中止传输': return $r('app.string.addon_delivery_file_changed');
    case '传输任务已满，已跳过本次事件；文件仍保留在本地': return $r('app.string.addon_delivery_queue');
    case '移动告警邮件已由服务器接受': return $r('app.string.addon_delivery_mail_sent');
    case '录像已上传；本地文件保留': return $r('app.string.addon_delivery_uploaded');
  }
  const reply = /失败（服务器状态 ([0-9]{3})）$/.exec(reason);
  return reply ? $r('app.string.addon_delivery_reply', reply[1]) :
    $r('app.string.addon_delivery_failed');
}

@Entry
@Component
struct Delivery {
  @State smtp: MailSettings = new MailSettings();
  @State upload: FtpSettings = new FtpSettings();
  @State mailEpoch: number = 0;
  @State ftpEpoch: number = 0;
  @State busy: boolean = false;
  @State ready: boolean = false;
  @State notice: ResourceStr = '';
  @State recent: ResourceStr = '';
  @State recentTitle: Resource = $r('app.string.addon_delivery_recent');
  onPageShow(): void {
    const result = lastDeliveryResult();
    const mail = result.startsWith('告警邮件：');
    const ftp = result.startsWith('录像上传：');
    this.recentTitle = mail ? $r('app.string.addon_delivery_recent_mail') : ftp ? $r('app.string.addon_delivery_recent_ftp') : $r('app.string.addon_delivery_recent');
    this.recent = result ? deliveryMessage(mail || ftp ? result.substring(result.indexOf('：') + 1) : result) : '';
  }
  aboutToAppear(): void { this.load(); }
  onBackPress(): boolean { return this.busy; }
  private async load(): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    try {
      this.smtp = await loadMail();
      this.upload = await loadFtp();
      this.mailEpoch++; this.ftpEpoch++;
      this.ready = true;
    } catch (err) { this.notice = deliveryMessage(safeFailure(err as Error)); }
    finally { this.busy = false; }
  }
  private destination(isMail: boolean, host: string, port: string): void {
    if (this.busy || !editDestination(isMail, isMail ? this.smtp : this.upload, host, port)) return;
    if (isMail) this.mailEpoch++; else this.ftpEpoch++;
  }
  private accountInput(isMail: boolean, epoch: number, user: boolean, value: string): void {
    if (this.busy || epoch !== (isMail ? this.mailEpoch : this.ftpEpoch)) return;
    editCredential(isMail, isMail ? this.smtp : this.upload, user, value);
  }
  private async run(isMail: boolean, test: boolean): Promise<void> {
    if (!this.ready || this.busy) return;
    this.busy = true;
    this.notice = test ? $r('app.string.addon_delivery_connecting') : $r('app.string.addon_delivery_saving');
    try {
      if (test) {
        if (isMail) {
          const epoch = this.mailEpoch, target = authTarget(true, this.smtp);
          await mail(new PlatformDialer(() => this.smtp.enabled && this.mailEpoch === epoch &&
            this.smtp.boundTo === target && authTarget(true, this.smtp) === target), this.smtp);
        } else {
          const target = endpoint(this.upload.host, this.upload.port), epoch = this.ftpEpoch;
          const dialer = new PlatformDialer(() => this.upload.enabled && this.ftpEpoch === epoch &&
            credentialsBound(false, this.upload) && target !== '' && this.upload.consent === target &&
            endpoint(this.upload.host, this.upload.port) === target);
          await ftp(dialer, this.upload);
        }
        this.notice = isMail ? $r('app.string.addon_delivery_mail_test_ok') : $r('app.string.addon_delivery_ftp_test_ok');
      } else {
        if (isMail) await saveMail(this.smtp);
        else await saveFtp(this.upload);
        this.notice = (isMail ? this.smtp.enabled : this.upload.enabled) ?
          $r('app.string.addon_delivery_saved_on', isMail ? 'SMTP' : 'FTP') : $r('app.string.addon_delivery_saved_off', isMail ? 'SMTP' : 'FTP');
      }
    } catch (err) { this.notice = deliveryMessage(safeFailure(err as Error)); }
    finally { this.busy = false; }
  }
  @Builder
  field(label: ResourceStr, value: () => string, update: (v: string) => void, hidden: boolean = false) {
    Column({ space: 4 }) {
      Text(label).fontSize(16).fontColor(ANDROID_TEXT).width('100%')
      TextInput({ text: value() }).attributeModifier(settingsField).type(hidden ? InputType.Password : InputType.Normal)
        .enableAutoFill(false).showPasswordIcon(false).maxLength(254).onChange(update)
    }.alignItems(HorizontalAlign.Start).width('100%')
  }
  @Builder
  switchRow(label: ResourceStr, value: () => boolean, update: (v: boolean) => void, available: () => boolean) {
    Row({ space: 12 }) {
      Text(label).fontSize(16).fontColor(ANDROID_TEXT).layoutWeight(1)
      Toggle({ type: ToggleType.Switch, isOn: value() }).selectedColor(ANDROID_ACCENT).enabled(available()).onChange(update)
    }.width('100%').constraintSize({ minHeight: 48 })
  }
  @Builder
  smtpCard() {
    Column({ space: 10 }) {
      SettingsSection($r('app.string.addon_delivery_mail_title'))
      Text($r('app.string.addon_delivery_mail_hint')).fontSize(14).fontColor(ANDROID_SECONDARY)
      Column({ space: 10 }) {
        this.field($r('app.string.addon_delivery_mail_host'), () => this.smtp.host, (v: string) => { this.destination(true, v, this.smtp.port); })
        this.field($r('app.string.addon_delivery_tls_port'), () => this.smtp.port, (v: string) => { this.destination(true, this.smtp.host, v); })
        ForEach([this.mailEpoch], (epoch: number) => {
          Column({ space: 10 }) {
            this.field($r('app.string.addon_delivery_mail_user'), () => this.smtp.user, (v: string) => { this.accountInput(true, epoch, true, v); })
            this.field($r('app.string.addon_delivery_mail_password'), () => this.smtp.pass, (v: string) => { this.accountInput(true, epoch, false, v); }, true)
            this.switchRow($r('app.string.addon_delivery_mail_enabled'), () => this.smtp.enabled, (v: boolean) => {
              if (epoch === this.mailEpoch) this.smtp.enabled = v && credentialsBound(true, this.smtp);
            }, () => credentialsBound(true, this.smtp))
          }.width('100%')
        }, (epoch: number) => epoch.toString())
        this.field($r('app.string.addon_delivery_from'), () => this.smtp.from, (v: string) => { this.smtp.from = v.trim(); })
        this.field($r('app.string.addon_delivery_to'), () => this.smtp.to, (v: string) => { this.smtp.to = v.trim(); })
        Button($r('app.string.addon_delivery_mail_test')).attributeModifier(settingsAction).enabled(this.smtp.enabled).onClick(() => { this.run(true, true); })
        Button($r('app.string.addon_delivery_mail_save')).attributeModifier(settingsAction).onClick(() => { this.run(true, false); })
      }.width('100%').alignItems(HorizontalAlign.Start).enabled(this.ready && !this.busy)
    }.alignItems(HorizontalAlign.Start).width('100%')
  }
  @Builder
  ftpCard() {
    Column({ space: 10 }) {
      SettingsSection($r('app.string.addon_delivery_ftp_title'))
      SettingsWarning($r('app.string.addon_delivery_ftp_warning'))
      Column({ space: 10 }) {
        this.field($r('app.string.addon_delivery_ftp_host'), () => this.upload.host, (v: string) => { this.destination(false, v, this.upload.port); })
        this.field($r('app.string.addon_delivery_control_port'), () => this.upload.port, (v: string) => { this.destination(false, this.upload.host, v); })
        ForEach([this.ftpEpoch], (epoch: number) => {
          Column({ space: 10 }) {
            this.field($r('app.string.addon_delivery_user'), () => this.upload.user, (v: string) => { this.accountInput(false, epoch, true, v); })
            this.field($r('app.string.addon_delivery_password'), () => this.upload.pass, (v: string) => { this.accountInput(false, epoch, false, v); }, true)
            this.switchRow($r('app.string.addon_delivery_ftp_enabled'), () => this.upload.enabled, (v: boolean) => {
              if (epoch === this.ftpEpoch) {
                this.upload.enabled = v && credentialsBound(false, this.upload);
                if (!this.upload.enabled) this.upload.consent = '';
              }
            }, () => credentialsBound(false, this.upload))
          }.width('100%')
        }, (epoch: number) => epoch.toString())
        this.field($r('app.string.addon_delivery_directory'), () => this.upload.dir, (v: string) => { this.upload.dir = v.trim(); })
        ForEach([this.ftpEpoch], (epoch: number) => {
          this.switchRow(endpoint(this.upload.host, this.upload.port) ?
            $r('app.string.addon_delivery_consent', endpoint(this.upload.host, this.upload.port)) :
            $r('app.string.addon_delivery_consent_default'),
            () => this.upload.consent !== '' && this.upload.consent === endpoint(this.upload.host, this.upload.port), (v: boolean) => {
              if (epoch === this.ftpEpoch) this.upload.consent = v && this.upload.enabled ? endpoint(this.upload.host, this.upload.port) : '';
            }, () => this.upload.enabled)
        }, (epoch: number) => epoch.toString())
        Button($r('app.string.addon_delivery_ftp_test')).attributeModifier(settingsAction).enabled(this.upload.enabled && this.upload.consent !== '' &&
          this.upload.consent === endpoint(this.upload.host, this.upload.port)).onClick(() => { this.run(false, true); })
        Button($r('app.string.addon_delivery_ftp_save')).attributeModifier(settingsAction).onClick(() => { this.run(false, false); })
      }.width('100%').alignItems(HorizontalAlign.Start).enabled(this.ready && !this.busy)
    }.alignItems(HorizontalAlign.Start).width('100%')
  }
  build() {
    WithTheme({ colorMode: ThemeColorMode.DARK }) {
      Column() {
        SettingsToolbar({ title: $r('app.string.addon_delivery_title'), backLabel: $r('app.string.addon_delivery_back'), canGoBack: !this.busy })
        if (this.notice) { Text(this.notice).fontSize(16).width('100%').padding(12).fontColor(ANDROID_TEXT) }
        if (!this.ready && !this.busy) {
          Button($r('app.string.addon_delivery_reload')).attributeModifier(settingsAction).onClick(() => { this.load(); })
        }
        Scroll() {
          Column({ space: 12 }) {
            Text($r('app.string.addon_delivery_hint')).fontSize(14).fontColor(ANDROID_SECONDARY)
            if (this.recent) {
              SettingsSection(this.recentTitle)
              Text(this.recent).fontSize(16).fontColor(ANDROID_TEXT)
            }
            Divider().color(ANDROID_DIVIDER)
            this.smtpCard()
            Divider().color(ANDROID_DIVIDER)
            this.ftpCard()
          }.alignItems(HorizontalAlign.Start).padding(16).width('100%')
        }.layoutWeight(1).width('100%').align(Alignment.TopStart)
      }.backgroundColor(ANDROID_BG).width('100%').height('100%')
    }
  }
}
