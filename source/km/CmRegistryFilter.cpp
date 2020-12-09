#include <ntddk.h>
#include <Ntstrsafe.h>

#include <intrin.h>

#include "StackStretch.h"

#include "constexpr.h"
#include "UndocumentedApi.h"

#include "config.h"
#include "LogicalFunction.h"

#include "MpqHashHelper.h"

#include "macro.h"

#include "MpqHash.h"

#include "CmRegistryFilter.h"

#include "MmProbeHelper.h"

// ��Ϊ����Ctx�����Ա���ʹ��_Global���������Լ�ͬ��������
#define _Global

#define DEF_REGISTER_CALL_BACK(Notify, Type)  \
NTSTATUS f##Notify(_In_ const CM_REGISTRY_FILTER_CONTEXT* ext, ULONG_PTR notify, _In_opt_  P##Type info)

#define ACTION_PROCESS(Item, ValueHash, Call, Action, ...)\
  ActionProcess(Item, ValueHash, Call, Action, __VA_ARGS__)

extern PCUNICODE_STRING FoundImageNameFromPebByBase(const _PEB* peb, PVOID Base);

ULONGLONG CurrentProcessFullNameHash(_In_ const CM_REGISTRY_FILTER_CONTEXT* Ctx) {
  auto Eprocess = PsGetCurrentProcess();
  auto Peb = PsGetProcessPeb(Eprocess);
  ULONGLONG H = MPQ_INVALID_HASH;

  if (!Peb) return 0;// System
  // Peb�ṹ��δʹ����ȫ��PAGE_SIZE��������PEB��ҳ�����sizeof(ULONGLONG)�б��浱ǰ����Hashֵ�����п��ٵĳ��ײ�����
  PULONGLONG PebNextPage = (PULONGLONG)((ULONG_PTR)Peb + PAGE_SIZE);
  if (!MmProbeForReadUser(&PebNextPage[-1], sizeof(PebNextPage[-1]), sizeof(ULONGLONG)))
    return H;

  if (PebNextPage[-1]) {
    H = PebNextPage[-1] ^ (ULONG_PTR)Eprocess;
    H ^= PsGetProcessCreateTimeQuadPart(Eprocess);
  } else {
    // ·��ʹ��Win32·����
    auto FullName = FoundImageNameFromPebByBase(Peb, PsGetProcessSectionBaseAddress(Eprocess));
    if (!MmProbeForReadUser((PVOID)FullName, sizeof(UNICODE_STRING), sizeof(ULONG_PTR))
      || !MmProbeForReadUser((PVOID)FullName->Buffer, FullName->MaximumLength, 1)) {
      ASSERT(0);
      return H;
    }
    Log("FullName %wZ", FullName);

    if (!FullName || FullName->Length > 260 * 2) {
      ASSERT(0);
      return H;
    }

    H = Hash((PUNICODE_STRING)FullName, Ctx->Table);
    if (!MmProbeForWriteUser(&PebNextPage[-1], sizeof(PebNextPage[-1]), sizeof(ULONGLONG)))
      return H;
    // �򵥵ķ��طŹ����ֿ���
    PebNextPage[-1] = H ^ PsGetProcessCreateTimeQuadPart(Eprocess) ^ (ULONG_PTR)Eprocess;
  }

  return H;
}

ULONGLONG FullKeyHash(const CM_REGISTRY_FILTER_CONTEXT* ext,
  PCUNICODE_STRING CompleteName, PVOID RootObject) {
  ULONGLONG h = MPQ_INVALID_HASH;
  if (!CompleteName) return h;

  if (!MmProbeForReadUser((PVOID)CompleteName, sizeof(UNICODE_STRING), sizeof(ULONG_PTR)))
    return h;
  if (!MmProbeForReadUser((PVOID)CompleteName->Buffer, CompleteName->MaximumLength, 1))
    return h;

  ASSERT((CompleteName->Buffer && CompleteName->Length)
    || (!CompleteName->Buffer && !CompleteName->Length));

  if (CompleteName->Buffer && CompleteName->Buffer[0] == L'\\') {
    //Log("FullKeyName1 %wZ", CompleteName);
    if (CompleteName->Length / 2 * 3 > 260 * 2) goto clean;

    h = Hash(CompleteName, ext->Table);
  } else if (RootObject) {
    PCUNICODE_STRING ParentName = nullptr;
    if (!NT_SUCCESS(CmCallbackGetKeyObjectID((PLARGE_INTEGER)&ext->Cookie, RootObject,
      nullptr, &ParentName))) {
      goto clean;
    }
    if (ParentName->Length + CompleteName->Length + (CompleteName->Length ? 2 : 0) > 260 * 2) {
      goto clean;
    }

    USHORT BufferSize = ParentName->Length + CompleteName->Length + 2;
    UCHAR* Buffer = (UCHAR*)alloca(BufferSize);
    UNICODE_STRING FullName = {0, BufferSize, (PWCH)Buffer};
    NTSTATUS st = RtlUnicodeStringCat(&FullName, ParentName);
    if (CompleteName->Length) {
      st |= RtlUnicodeStringCatString(&FullName, L"\\");
      st |= RtlUnicodeStringCat(&FullName, CompleteName);
    }

    if (!NT_SUCCESS(st)) {
      goto clean;
    }

    h = Hash(&FullName, ext->Table);
  }
clean:
  return h;
}

DEF_REGISTER_CALL_BACK(RegNtPreCreateKeyEx, REG_CREATE_KEY_INFORMATION) {
  // ���ڰ�����֮ǰ����Ҫע���������⡣
  auto h = FullKeyHash(ext, info->CompleteName, info->RootObject);
  if (h == MPQ_INVALID_HASH) return STATUS_SUCCESS;

  auto H = CurrentProcessFullNameHash(ext);
  if (H == MPQ_INVALID_HASH) return STATUS_SUCCESS;

  auto item = GetConfigItem(ext->Config, h);
  auto GlobalCheck = NOT_INITIALIZED;
  if (!item && ext->DrvDelegateRoutine) {
    auto Index = GlobalCheck = GlobalWhiteList(
        ext->Config,
        CONFIG_GLOBAL_INDEX_CONFIG,
        CONFIG_LOCAL_INDEX_WHITE,
        H);
    // ����֮ǰ�߼�
    if (Index != INDEX_IN_LIST) {
      ext->DrvDelegateRoutine(DRV_DELEGATE_NOT_RULE, nullptr, H, h);
      return STATUS_SUCCESS;
    }
    // ֻ���sxs time value��ͨ����ǰ��������ȫ�ְ�������
    // ��ΪitemΪnullptr��û�м�������ı�Ҫ�����ء�
    ext->DrvDelegateRoutine(DRV_DELEGATE_SXS_TIME_VALUE, nullptr, H, h);
    return STATUS_SUCCESS;
  }

  // �ڲ�����Flags������£��ֱ���������������ڰ���������Ź���
  ULONGLONG Flags = 0;
  {
    ULONGLONG* Value = (ULONGLONG*)GetConfigItemEntry(item, CONFIG_LOCAL_INDEX_WHITE_FLAGS, nullptr);
    Flags = Value ? *Value : 0;
  }

  auto Check = WhiteList(item, CONFIG_LOCAL_INDEX_WHITE, H);
  if (Check != NON_INDEX_EXIST && (Check ^ !!(Flags & WHITE_FLAG_INVERSE_LOCAL))) {
    return STATUS_SUCCESS;
  }

  if (!(Flags & WHITE_FLAG_IGNORE_GLOBAL)) {
    GlobalCheck = GlobalCheck == NOT_INITIALIZED ?
      GlobalWhiteList(ext->Config, CONFIG_GLOBAL_INDEX_CONFIG, CONFIG_LOCAL_INDEX_WHITE, H) : GlobalCheck;

    if (INDEX_IN_LIST == GlobalCheck) {
      return STATUS_SUCCESS;
    }
  }

  if (ext->DrvDelegateRoutine && !ext->DrvDelegateRoutine(DRV_DELEGATE_NOT_WHITE, (PVOID)item, H, h))
    return STATUS_SUCCESS;

  info->CallContext = (PVOID)item;

  PCUNICODE_STRING RootName = nullptr;
  if (info->RootObject) {
    CmCallbackGetKeyObjectID((PLARGE_INTEGER)&ext->Cookie, info->RootObject, nullptr, &RootName);
  }
  // Log("Key CallContext %wZ %wZ", RootName, info->CompleteName);
  return ACTION_PROCESS(item, 0, notify, RegActionKeyRedirect, RootName, &info->CompleteName);
}

DEF_REGISTER_CALL_BACK(RegNtPostCreateKeyEx, REG_POST_OPERATION_INFORMATION) {
  notify;
  if (info->Status != STATUS_SUCCESS) return STATUS_SUCCESS;

  if (info->CallContext) {
    // Log("Key CallCountext %p", info->CallContext);
    auto st = CmSetCallbackObjectContext(info->Object, (PLARGE_INTEGER)&ext->Cookie, info->CallContext, nullptr);
    ASSERT(st == STATUS_SUCCESS);
    return st;
  }
  return STATUS_SUCCESS;
}

DEF_REGISTER_CALL_BACK(RegNtPreQueryValueKey, REG_QUERY_VALUE_KEY_INFORMATION) {
  CONST CONFIG_ITEM* item = (CONST CONFIG_ITEM*)info->ObjectContext;
  if (item == nullptr) return STATUS_SUCCESS;

  // Log("%p, ValueName %wZ", item, info->ValueName);
  if (!MmProbeForReadUser((PVOID)info->ValueName, sizeof(UNICODE_STRING), sizeof(ULONG_PTR)))
    return STATUS_SUCCESS;
  if (!MmProbeForReadUser((PVOID)info->ValueName->Buffer, info->ValueName->MaximumLength, 1))
    return STATUS_SUCCESS;

  ASSERT(!info->ValueName || !info->ValueName->Length
    || info->ValueName->Buffer[info->ValueName->Length / sizeof(wchar_t) -1]);

  auto h = Hash(info->ValueName, ext->Table);
  if (h == MPQ_INVALID_HASH) return STATUS_SUCCESS;

  return ACTION_PROCESS(item, h, notify, RegActionValueRevalue,
    info->KeyValueInformationClass, info->KeyValueInformation,
    info->Length, info->ValueName->Buffer, info->ValueName->Length,
    info->ResultLength);
}

DEF_REGISTER_CALL_BACK(RegNtPreEnumerateValueKey, REG_ENUMERATE_KEY_INFORMATION) {
  ext;
  CONST CONFIG_ITEM* item = (CONST CONFIG_ITEM*)info->ObjectContext;
  if (item == nullptr) return STATUS_SUCCESS;

  auto st = ACTION_PROCESS(item, info->Index, notify, RegActionValueReenum,
    info->KeyInformationClass, info->KeyInformation, info->Length,
    info->ResultLength);
  return st;
}

DEF_REGISTER_CALL_BACK(RegNtPreQueryKey, REG_QUERY_KEY_INFORMATION) {
  notify, ext;
  CONST CONFIG_ITEM* item = (CONST CONFIG_ITEM*)info->ObjectContext;
  if (item == nullptr) return STATUS_SUCCESS;

  auto st = ACTION_PROCESS(item, 0, notify, RegActionQueryKeyRedirect,
    info->KeyInformationClass, info->KeyInformation, info->Length,
    info->ResultLength);

  return st;
}

DEF_REGISTER_CALL_BACK(RegNtPostQueryKey, REG_POST_OPERATION_INFORMATION) {
  notify, ext;
  CONST CONFIG_ITEM* item = (CONST CONFIG_ITEM*)info->ObjectContext;
  if (item == nullptr) return STATUS_SUCCESS;

  return STATUS_SUCCESS;
}

// for PublisherPolicyChangeTime hardcode.
DEF_REGISTER_CALL_BACK(RegNtPostQueryValueKey, REG_POST_OPERATION_INFORMATION) {
  notify, ext;
  CONST CONFIG_ITEM* item = (CONST CONFIG_ITEM*)info->ObjectContext;
  if (item || info->ReturnStatus != STATUS_SUCCESS) return STATUS_SUCCESS;

  REG_QUERY_VALUE_KEY_INFORMATION* pre = (REG_QUERY_VALUE_KEY_INFORMATION*)info->PreInformation;
  if (*pre->ResultLength != 20
    || pre->KeyValueInformationClass != KeyValuePartialInformation)
    return STATUS_SUCCESS;
  PKEY_VALUE_PARTIAL_INFORMATION value = (PKEY_VALUE_PARTIAL_INFORMATION)pre->KeyValueInformation;
  if (value->Type != REG_QWORD
    || value->DataLength != sizeof(ULONGLONG)) return STATUS_SUCCESS;

  if (!ext->DrvDelegateRoutine(DRV_DELEGATE_CAPTURE_CSRSS1,
    nullptr, 0, 0)) {
    return STATUS_SUCCESS;
  }
  Log("Can Capture csrss.exe");
  UNICODE_STRING Empty = {0};
  ULONGLONG XorValue = Hash(pre->ValueName, ext->Table);
  BOOLEAN bl = ext->DrvDelegateRoutine(DRV_DELEGATE_CAPTURE_CSRSS2,
    &XorValue,
    CurrentProcessFullNameHash(ext),
    FullKeyHash(ext, &Empty, info->Object));
  if (!bl) return STATUS_SUCCESS;

  *(PULONGLONG)value->Data ^= XorValue;

  return STATUS_SUCCESS;
}

DEF_REGISTER_CALL_BACK(RegNtPreSetValueKey, REG_SET_VALUE_KEY_INFORMATION) {
  CONST CONFIG_ITEM* item = (CONST CONFIG_ITEM*)info->ObjectContext;
  if (item == nullptr) return STATUS_SUCCESS;

  if (!MmProbeForReadUser((PVOID)info->ValueName, sizeof(UNICODE_STRING), sizeof(ULONG_PTR)))
    return STATUS_SUCCESS;
  if (!MmProbeForReadUser((PVOID)info->ValueName->Buffer, info->ValueName->MaximumLength, 1))
    return STATUS_SUCCESS;

  auto h = Hash(info->ValueName, ext->Table);
  if (h == MPQ_INVALID_HASH) return STATUS_SUCCESS;

  return ACTION_PROCESS(item, h, notify, RegActionSetValueNotice,
    &info->Type, &info->Data, &info->DataSize);
}

DEF_REGISTER_CALL_BACK(RegNtCallbackObjectContextCleanup, REG_CALLBACK_CONTEXT_CLEANUP_INFORMATION) {
  notify, info, ext;
  return STATUS_SUCCESS;
}
constexpr auto fRegNtPreOpenKeyEx = fRegNtPreCreateKeyEx;

constexpr auto fRegNtPostOpenKeyEx = fRegNtPostCreateKeyEx;

#define DEF_CB_ENTRY(Type)    f##Type

typedef NTSTATUS(*CallbackInternalType)(PCM_REGISTRY_FILTER_CONTEXT, ULONG_PTR, PVOID);

constexpr VOID* CallbackArray[MaxRegNtNotifyClass]{
  //RegNtDeleteKey,
  //RegNtPreDeleteKey = RegNtDeleteKey,
  nullptr,
  //RegNtSetValueKey,
  //RegNtPreSetValueKey = RegNtSetValueKey,
  DEF_CB_ENTRY(RegNtPreSetValueKey),//nullptr,
  //RegNtDeleteValueKey,
  //RegNtPreDeleteValueKey = RegNtDeleteValueKey,
  nullptr,
  //RegNtSetInformationKey,
  //RegNtPreSetInformationKey = RegNtSetInformationKey,
  nullptr,
  //RegNtRenameKey,
  //RegNtPreRenameKey = RegNtRenameKey,
  nullptr,
  //RegNtEnumerateKey,
  //RegNtPreEnumerateKey = RegNtEnumerateKey,
  nullptr,
  //RegNtEnumerateValueKey,
  //RegNtPreEnumerateValueKey = RegNtEnumerateValueKey,
  DEF_CB_ENTRY(RegNtPreEnumerateValueKey),//nullptr,
  //RegNtQueryKey,
  //RegNtPreQueryKey = RegNtQueryKey,
  DEF_CB_ENTRY(RegNtPreQueryKey),//nullptr,
  //RegNtQueryValueKey,
  //RegNtPreQueryValueKey = RegNtQueryValueKey,
  DEF_CB_ENTRY(RegNtPreQueryValueKey),//nullptr,
  //RegNtQueryMultipleValueKey,
  //RegNtPreQueryMultipleValueKey = RegNtQueryMultipleValueKey,
  nullptr,
  //RegNtPreCreateKey,
  nullptr,
  //RegNtPostCreateKey,
  nullptr,
  //RegNtPreOpenKey,
  nullptr,
  //RegNtPostOpenKey,
  nullptr,
  //RegNtKeyHandleClose,
  //RegNtPreKeyHandleClose = RegNtKeyHandleClose,
  nullptr,
  ////
  //// .Net only
  ////
  //RegNtPostDeleteKey,
  nullptr,
  //RegNtPostSetValueKey,
  nullptr,
  //RegNtPostDeleteValueKey,
  nullptr,
  //RegNtPostSetInformationKey,
  nullptr,
  //RegNtPostRenameKey,
  nullptr,
  //RegNtPostEnumerateKey,
  nullptr,
  //RegNtPostEnumerateValueKey,
  nullptr,
  //RegNtPostQueryKey,
  DEF_CB_ENTRY(RegNtPostQueryKey),//nullptr,
  //RegNtPostQueryValueKey,
  DEF_CB_ENTRY(RegNtPostQueryValueKey),//nullptr,
  //RegNtPostQueryMultipleValueKey,
  nullptr,
  //RegNtPostKeyHandleClose,
  nullptr,
  //RegNtPreCreateKeyEx,
  DEF_CB_ENTRY(RegNtPreCreateKeyEx),//nullptr,
  //RegNtPostCreateKeyEx,
  DEF_CB_ENTRY(RegNtPostCreateKeyEx),//nullptr,
  //RegNtPreOpenKeyEx,
  DEF_CB_ENTRY(RegNtPreOpenKeyEx),//nullptr
  //RegNtPostOpenKeyEx,
  DEF_CB_ENTRY(RegNtPostOpenKeyEx),//nullptr,
  ////
  //// new to Windows Vista
  ////
  //RegNtPreFlushKey,
  nullptr,
  //RegNtPostFlushKey,
  nullptr,
  //RegNtPreLoadKey,
  nullptr,
  //RegNtPostLoadKey,
  nullptr,
  //RegNtPreUnLoadKey,
  nullptr,
  //RegNtPostUnLoadKey,
  nullptr,
  //RegNtPreQueryKeySecurity,
  nullptr,
  //RegNtPostQueryKeySecurity,
  nullptr,
  //RegNtPreSetKeySecurity,
  nullptr,
  //RegNtPostSetKeySecurity,
  nullptr,
  ////
  //// per-object context cleanup
  ////
  //RegNtCallbackObjectContextCleanup,
  DEF_CB_ENTRY(RegNtCallbackObjectContextCleanup),// nullptr,
  ////
  //// new in Vista SP2 
  ////
  //RegNtPreRestoreKey,
  nullptr,
  //RegNtPostRestoreKey,
  nullptr,
  //RegNtPreSaveKey,
  nullptr,
  //RegNtPostSaveKey,
  nullptr,
  //RegNtPreReplaceKey,
  nullptr,
  //RegNtPostReplaceKey,
  nullptr,
  ////
  //// new to Windows 10
  ////
  //RegNtPreQueryKeyName,
  nullptr,
  //RegNtPostQueryKeyName,
  nullptr,
  //MaxRegNtNotifyClass //should always be the last enum
};

NTSTATUS RegistryCallbackInternal(
  _In_      PCM_REGISTRY_FILTER_CONTEXT Ctx,
  _In_opt_  ULONG_PTR notify,
  _In_opt_  PVOID Argument2) {
  NTSTATUS st = STATUS_SUCCESS;

  __try {
    st = ((CallbackInternalType)CallbackArray[notify])(Ctx, notify, Argument2);
  } __except (Log("GetExceptionInformation %p", GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    Log("%p, %p, %p", Ctx, notify, Argument2);
    ASSERT(0);
  }
  return st;
}

NTSTATUS RegistryCallback(
  _In_      PCM_REGISTRY_FILTER_CONTEXT Ctx,
  _In_opt_  ULONG_PTR notify,
  _In_opt_  PVOID Argument2) {
  PAGED_CODE();
  NTSTATUS st = STATUS_SUCCESS;

  if (!Ctx) return st;
  if (KeGetCurrentIrql() > APC_LEVEL) return st;

  if (notify >= MaxRegNtNotifyClass || !CallbackArray[notify]) return st;
  ULONG_PTR remaining = IoGetRemainingStackSize();
  if (remaining < PAGE_SIZE) {
    return st;
  }
  st = RegistryCallbackInternal(Ctx, notify, Argument2);

  return st;
}

// #pragma code_seg("PAGE")
NTSTATUS UninitializeRegistryFilter(PCM_REGISTRY_FILTER_CONTEXT Context) {
  if (!Context) return STATUS_UNSUCCESSFUL;

#ifdef DBG
  int test = 0;
  if (!test) {
#endif // DBG

  NTSTATUS st = CmUnRegisterCallback(Context->Cookie);
  ASSERT(st == STATUS_SUCCESS);
  ExFreePool(Context);
  return st;

#ifdef DBG
  }
  ExFreePool(Context);
  return STATUS_UNSUCCESSFUL;
#endif // DBG
}

NTSTATUS InitializeRegistryFilter(
  PDRIVER_OBJECT DrvObj,
  WCHAR Altitude[8],
  PNPAGED_LOOKASIDE_LIST LookAsideList,
  CONST CONFIG_TABLE* Config,
  CONST VOID* Table, DRV_DELEGATE_ROUTINE DrvDelegateRoutine,
  PCM_REGISTRY_FILTER_CONTEXT* Filter) {
  PCM_REGISTRY_FILTER_CONTEXT Ctx =
    (PCM_REGISTRY_FILTER_CONTEXT)MALLOC(sizeof(CM_REGISTRY_FILTER_CONTEXT));
  if (!Ctx) return STATUS_UNSUCCESSFUL;

  // ��ȡ��filter stack top
  // ����RtlCompareAltitudes������,��������Length���Ա�֤����Խ�ߣ����Խṹ����_GLOBAL_ENVIRONMENT_CORE::Altitude����Table��
  /* RtlCompareAltitudes��Ϊ��
   * �ַ�������ʮ���Ʊ�ʾ��float�ַ��������бȽϣ���ʱ��δ�ж��ַ��Ƿ���L'0' - L'9'���䡣
   * 1.���ַ���ͷ����ɨ�裬���ҵ���һ����ΪL'0'���ַ���
   * 2.����ɨ�裬����L'.'��λ��
   * 3.�Ƚ��������֣�Խ����ʾ����Խ���������һ�����ַ����жԱȡ�
   * 4.�����������һ�£������Ƚ�С�����֡�
   * 5.����ֵ��strcmpһ�¡�
  */
  UNICODE_STRING Altiude = {
    MPQ_HASH_TABLE_SIZE * sizeof(ULONG),
    MPQ_HASH_TABLE_SIZE * sizeof(ULONG),
    (PWCH)Altitude
  };

  NTSTATUS st = STATUS_FLT_INSTANCE_ALTITUDE_COLLISION;
  //CmRegisterCallbackEx((PEX_CALLBACK_FUNCTION)RegistryCallback, &Altiude, DrvObj, Ctx, &Ctx->Cookie, nullptr);

  Ctx->Config = Config;
  Ctx->Table = Table;
  Ctx->DrvDelegateRoutine = DrvDelegateRoutine;
  Ctx->StackStretchList = LookAsideList;

  for (int i = 1; st == STATUS_FLT_INSTANCE_ALTITUDE_COLLISION && i <= 3; i++) {
    st = CmRegisterCallbackEx((PEX_CALLBACK_FUNCTION)RegistryCallback,
      &Altiude, DrvObj, Ctx, &Ctx->Cookie, nullptr);
    // ��ֹSTATUS_FLT_INSTANCE_ALTITUDE_COLLISION��
    Altitude[7] += 1;
    if (Altitude[7] < L'0' || Altitude[7] > L'9') {
      Altitude[7] = L'0';
    }
  }

  if (!NT_SUCCESS(st)) {
    ExFreePool(Ctx);
  } else {
    ASSERT(*Filter == nullptr);
    *Filter = Ctx;
  }
  Log("st %08x", st);
  return st;
}