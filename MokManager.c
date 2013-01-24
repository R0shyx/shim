#include <efi.h>
#include <efilib.h>
#include <Library/BaseCryptLib.h>
#include <openssl/x509.h>
#include "shim.h"
#include "signature.h"
#include "PeImage.h"
#include "PasswordCrypt.h"

#include "include/console.h"
#include "include/simple_file.h"

#define PASSWORD_MAX 256
#define PASSWORD_MIN 1
#define SB_PASSWORD_LEN 16

#ifndef SHIM_VENDOR
#define SHIM_VENDOR L"Shim"
#endif

#define EFI_VARIABLE_APPEND_WRITE 0x00000040

typedef struct {
	UINT32 MokSize;
	UINT8 *Mok;
	EFI_GUID Type;
} __attribute__ ((packed)) MokListNode;

typedef struct {
	UINT32 MokSBState;
	UINT32 PWLen;
	CHAR16 Password[SB_PASSWORD_LEN];
} __attribute__ ((packed)) MokSBvar;

static EFI_STATUS get_variable (CHAR16 *name, EFI_GUID guid, UINT32 *attributes,
				UINTN *size, void **buffer)
{
	EFI_STATUS efi_status;
	char allocate = !(*size);

	efi_status = uefi_call_wrapper(RT->GetVariable, 5, name, &guid,
				       attributes, size, buffer);

	if (efi_status != EFI_BUFFER_TOO_SMALL || !allocate) {
		return efi_status;
	}

	*buffer = AllocatePool(*size);

	if (!*buffer) {
		console_notify(L"Unable to allocate variable buffer");
		return EFI_OUT_OF_RESOURCES;
	}

	efi_status = uefi_call_wrapper(RT->GetVariable, 5, name, &guid,
				       attributes, size, *buffer);

	return efi_status;
}

static EFI_INPUT_KEY get_keystroke (void)
{
	EFI_INPUT_KEY key;
	UINTN EventIndex;

	uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey,
			  &EventIndex);
	uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);

	return key;
}

static EFI_STATUS get_sha1sum (void *Data, int DataSize, UINT8 *hash)
{
	EFI_STATUS status;
	unsigned int ctxsize;
	void *ctx = NULL;

	ctxsize = Sha1GetContextSize();
	ctx = AllocatePool(ctxsize);

	if (!ctx) {
		console_notify(L"Unable to allocate memory for hash context");
		return EFI_OUT_OF_RESOURCES;
	}

	if (!Sha1Init(ctx)) {
		console_notify(L"Unable to initialise hash");
		status = EFI_OUT_OF_RESOURCES;
		goto done;
	}

	if (!(Sha1Update(ctx, Data, DataSize))) {
		console_notify(L"Unable to generate hash");
		status = EFI_OUT_OF_RESOURCES;
		goto done;
	}

	if (!(Sha1Final(ctx, hash))) {
		console_notify(L"Unable to finalise hash");
		status = EFI_OUT_OF_RESOURCES;
		goto done;
	}

	status = EFI_SUCCESS;
done:
	return status;
}

static UINT32 count_keys(void *Data, UINTN DataSize)
{
	EFI_SIGNATURE_LIST *CertList = Data;
	EFI_GUID CertType = EfiCertX509Guid;
	EFI_GUID HashType = EfiHashSha256Guid;
	UINTN dbsize = DataSize;
	UINT32 MokNum = 0;

	while ((dbsize > 0) && (dbsize >= CertList->SignatureListSize)) {
		if ((CompareGuid (&CertList->SignatureType, &CertType) != 0) &&
		    (CompareGuid (&CertList->SignatureType, &HashType) != 0)) {
			console_notify(L"Doesn't look like a key or hash");
			dbsize -= CertList->SignatureListSize;
			CertList = (EFI_SIGNATURE_LIST *) ((UINT8 *) CertList +
						  CertList->SignatureListSize);
			continue;
		}

		if ((CompareGuid (&CertList->SignatureType, &CertType) != 0) &&
		    (CertList->SignatureSize != 48)) {
			console_notify(L"Doesn't look like a valid hash");
			dbsize -= CertList->SignatureListSize;
			CertList = (EFI_SIGNATURE_LIST *) ((UINT8 *) CertList +
						  CertList->SignatureListSize);
			continue;
		}

		MokNum++;
		dbsize -= CertList->SignatureListSize;
		CertList = (EFI_SIGNATURE_LIST *) ((UINT8 *) CertList +
						  CertList->SignatureListSize);
	}

	return MokNum;
}

static MokListNode *build_mok_list(UINT32 num, void *Data, UINTN DataSize) {
	MokListNode *list;
	EFI_SIGNATURE_LIST *CertList = Data;
	EFI_SIGNATURE_DATA *Cert;
	EFI_GUID CertType = EfiCertX509Guid;
	EFI_GUID HashType = EfiHashSha256Guid;
	UINTN dbsize = DataSize;
	UINTN count = 0;

	list = AllocatePool(sizeof(MokListNode) * num);

	if (!list) {
		console_notify(L"Unable to allocate MOK list");
		return NULL;
	}

	while ((dbsize > 0) && (dbsize >= CertList->SignatureListSize)) {
		if ((CompareGuid (&CertList->SignatureType, &CertType) != 0) &&
		    (CompareGuid (&CertList->SignatureType, &HashType) != 0)) {
			dbsize -= CertList->SignatureListSize;
			CertList = (EFI_SIGNATURE_LIST *)((UINT8 *) CertList +
						  CertList->SignatureListSize);
			continue;
		}

		if ((CompareGuid (&CertList->SignatureType, &HashType) == 0) &&
		    (CertList->SignatureSize != 48)) {
			dbsize -= CertList->SignatureListSize;
			CertList = (EFI_SIGNATURE_LIST *)((UINT8 *) CertList +
						  CertList->SignatureListSize);
			continue;
		}

		Cert = (EFI_SIGNATURE_DATA *) (((UINT8 *) CertList) +
		  sizeof (EFI_SIGNATURE_LIST) + CertList->SignatureHeaderSize);

		list[count].MokSize = CertList->SignatureSize - sizeof(EFI_GUID);
		list[count].Mok = (void *)Cert->SignatureData;
		list[count].Type = CertList->SignatureType;

		count++;
		dbsize -= CertList->SignatureListSize;
		CertList = (EFI_SIGNATURE_LIST *) ((UINT8 *) CertList +
						  CertList->SignatureListSize);
	}

	return list;
}

static CHAR16* get_x509_name (X509_NAME *X509Name, CHAR16 *name)
{
	char *str;
	CHAR16 *ret = NULL;

	str = X509_NAME_oneline(X509Name, NULL, 0);
	if (str) {
		ret = PoolPrint(L"%s: %a", name, str);
		OPENSSL_free(str);
	}
	return ret;
}

static const char *mon[12]= {
"Jan","Feb","Mar","Apr","May","Jun",
"Jul","Aug","Sep","Oct","Nov","Dec"
};

static void print_x509_GENERALIZEDTIME_time (ASN1_TIME *time, CHAR16 *time_string)
{
	char *v;
	int gmt = 0;
	int i;
	int y = 0,M = 0,d = 0,h = 0,m = 0,s = 0;
	char *f = NULL;
	int f_len = 0;

	i=time->length;
	v=(char *)time->data;

	if (i < 12)
		goto error;

	if (v[i-1] == 'Z')
		gmt=1;

	for (i=0; i<12; i++) {
		if ((v[i] > '9') || (v[i] < '0'))
			goto error;
	}

	y = (v[0]-'0')*1000+(v[1]-'0')*100 + (v[2]-'0')*10+(v[3]-'0');
	M = (v[4]-'0')*10+(v[5]-'0');

	if ((M > 12) || (M < 1))
		goto error;

	d = (v[6]-'0')*10+(v[7]-'0');
	h = (v[8]-'0')*10+(v[9]-'0');
	m = (v[10]-'0')*10+(v[11]-'0');

	if (time->length >= 14 &&
	    (v[12] >= '0') && (v[12] <= '9') &&
	    (v[13] >= '0') && (v[13] <= '9')) {
		s = (v[12]-'0')*10+(v[13]-'0');
		/* Check for fractions of seconds. */
		if (time->length >= 15 && v[14] == '.')	{
			int l = time->length;
			f = &v[14];	/* The decimal point. */
			f_len = 1;
			while (14 + f_len < l && f[f_len] >= '0' &&
			       f[f_len] <= '9')
				++f_len;
		}
	}

	SPrint(time_string, 0, L"%a %2d %02d:%02d:%02d%.*a %d%a",
	       mon[M-1], d, h, m, s, f_len, f, y, (gmt)?" GMT":"");
error:
	return;
}

static void print_x509_UTCTIME_time (ASN1_TIME *time, CHAR16 *time_string)
{
	char *v;
	int gmt=0;
	int i;
	int y = 0,M = 0,d = 0,h = 0,m = 0,s = 0;

	i=time->length;
	v=(char *)time->data;

	if (i < 10)
		goto error;

	if (v[i-1] == 'Z')
		gmt=1;

	for (i=0; i<10; i++)
		if ((v[i] > '9') || (v[i] < '0'))
			goto error;

	y = (v[0]-'0')*10+(v[1]-'0');

	if (y < 50)
		y+=100;

	M = (v[2]-'0')*10+(v[3]-'0');

	if ((M > 12) || (M < 1))
		goto error;

	d = (v[4]-'0')*10+(v[5]-'0');
	h = (v[6]-'0')*10+(v[7]-'0');
	m = (v[8]-'0')*10+(v[9]-'0');

	if (time->length >=12 &&
	    (v[10] >= '0') && (v[10] <= '9') &&
	    (v[11] >= '0') && (v[11] <= '9'))
		s = (v[10]-'0')*10+(v[11]-'0');

	SPrint(time_string, 0, L"%a %2d %02d:%02d:%02d %d%a",
	       mon[M-1], d, h, m, s, y+1900, (gmt)?" GMT":"");
error:
	return;
}

static CHAR16* get_x509_time (ASN1_TIME *time, CHAR16 *name)
{
	CHAR16 time_string[30];

	if (time->type == V_ASN1_UTCTIME) {
		print_x509_UTCTIME_time(time, time_string);
	} else if (time->type == V_ASN1_GENERALIZEDTIME) {
		print_x509_GENERALIZEDTIME_time(time, time_string);
	} else {
		time_string[0] = '\0';
	}

	return PoolPrint(L"%s: %s", name, time_string);
}

static void show_x509_info (X509 *X509Cert, UINT8 *hash)
{
	ASN1_INTEGER *serial;
	BIGNUM *bnser;
	unsigned char hexbuf[30];
	X509_NAME *X509Name;
	ASN1_TIME *time;
	CHAR16 *issuer = NULL;
	CHAR16 *subject = NULL;
	CHAR16 *from = NULL;
	CHAR16 *until = NULL;
	POOL_PRINT hash_string1;
	POOL_PRINT hash_string2;
	POOL_PRINT serial_string;
	int fields = 0;
	CHAR16 **text;
	int i = 0;

	ZeroMem(&hash_string1, sizeof(hash_string1));
	ZeroMem(&hash_string2, sizeof(hash_string2));
	ZeroMem(&serial_string, sizeof(serial_string));

	serial = X509_get_serialNumber(X509Cert);
	if (serial) {
		int i, n;
		bnser = ASN1_INTEGER_to_BN(serial, NULL);
		n = BN_bn2bin(bnser, hexbuf);
		CatPrint(&serial_string, L"Serial Number:");
		for (i = 0; i < n; i++) {
			CatPrint(&serial_string, L"%02x:", hexbuf[i]);
		}
	}

	if (serial_string.str)
		fields++;

	X509Name = X509_get_issuer_name(X509Cert);
	if (X509Name) {
		issuer = get_x509_name(X509Name, L"Issuer");
		if (issuer)
			fields++;
	}

	X509Name = X509_get_subject_name(X509Cert);
	if (X509Name) {
		subject = get_x509_name(X509Name, L"Subject");
		if (subject)
			fields++;
	}

	time = X509_get_notBefore(X509Cert);
	if (time) {
		from = get_x509_time(time, L"Validity from");
		if (time)
			fields++;
	}

	time = X509_get_notAfter(X509Cert);
	if (time) {
		until = get_x509_time(time, L"Validity till");
		if (until)
			fields++;
	}

#if 0
	CatPrint(&hash_string1, L"SHA1 Fingerprint: ");
	for (i=0; i<10; i++)
		CatPrint(&hash_string1, L"%02x ", hash[i]);
	for (i=10; i<20; i++)
		CatPrint(&hash_string2, L"%02x ", hash[i]);

	if (hash_string1.str)
		fields++;

	if (hash_string2.str)
		fields++;
#endif
	if (!fields)
		return;

	text = AllocateZeroPool(sizeof(CHAR16 *) * (fields + 1));
	if (serial_string.str) {
		text[i] = serial_string.str;
		i++;
	}
	if (issuer) {
		text[i] = issuer;
		i++;
	}
	if (subject) {
		text[i] = subject;
		i++;
	}
	if (from) {
		text[i] = from;
		i++;
	}
	if (until) {
		text[i] = until;
		i++;
	}
	if (hash_string1.str) {
		text[i] = hash_string1.str;
		i++;
	}
	if (hash_string2.str) {
		text[i] = hash_string2.str;
		i++;
	}
	text[i] = NULL;

	console_alertbox(text);

	for (i=0; text[i] != NULL; i++)
		FreePool(text[i]);

	FreePool(text);
}

static void show_mok_info (void *Mok, UINTN MokSize)
{
	EFI_STATUS efi_status;
	UINT8 hash[SHA1_DIGEST_SIZE];
	unsigned int i;
	X509 *X509Cert;

	if (!Mok || MokSize == 0)
		return;

	if (MokSize != SHA256_DIGEST_SIZE) {
		efi_status = get_sha1sum(Mok, MokSize, hash);

		if (efi_status != EFI_SUCCESS) {
			console_notify(L"Failed to compute MOK fingerprint");
			return;
		}

		if (X509ConstructCertificate(Mok, MokSize,
				 (UINT8 **) &X509Cert) && X509Cert != NULL) {
			show_x509_info(X509Cert, hash);
			X509_free(X509Cert);
		} else {
			console_notify(L"Not a valid X509 certificate");
			return;
		}
	} else {
		Print(L"SHA256 hash:\n   ");
		for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
			Print(L" %02x", ((UINT8 *)Mok)[i]);
			if (i % 10 == 9)
				Print(L"\n   ");
		}
		Print(L"\n");
	}
}

static EFI_STATUS list_keys (void *KeyList, UINTN KeyListSize, CHAR16 *title)
{
	UINT32 MokNum = 0;
	MokListNode *keys = NULL;
	INTN key_num = 0;
	CHAR16 **menu_strings;
	int i;

	if (KeyListSize < (sizeof(EFI_SIGNATURE_LIST) +
			   sizeof(EFI_SIGNATURE_DATA))) {
		console_notify(L"No MOK keys found");
		return 0;
	}

	MokNum = count_keys(KeyList, KeyListSize);
	keys = build_mok_list(MokNum, KeyList, KeyListSize);

	if (!keys) {
		console_notify(L"Failed to construct key list");
		return 0;
	}

	menu_strings = AllocateZeroPool(sizeof(CHAR16 *) * (MokNum + 2));

	if (!menu_strings)
		return EFI_OUT_OF_RESOURCES;

	for (i=0; i<MokNum; i++) {
		menu_strings[i] = PoolPrint(L"View key %d", i);
	}
	menu_strings[i] = StrDuplicate(L"Continue");

	menu_strings[i+1] = NULL;

	while (key_num < MokNum) {
		key_num = console_select((CHAR16 *[]){ title, NULL },
					 menu_strings, 0);

		if (key_num < MokNum)
			show_mok_info(keys[key_num].Mok, keys[key_num].MokSize);
	}

	for (i=0; menu_strings[i] != NULL; i++)
		FreePool(menu_strings[i]);

	FreePool(menu_strings);

	FreePool(keys);

	return EFI_SUCCESS;
}

static UINT8 get_line (UINT32 *length, CHAR16 *line, UINT32 line_max, UINT8 show)
{
	EFI_INPUT_KEY key;
	int count = 0;

	do {
		key = get_keystroke();

		if ((count >= line_max &&
		     key.UnicodeChar != CHAR_BACKSPACE) ||
		    key.UnicodeChar == CHAR_NULL ||
		    key.UnicodeChar == CHAR_TAB  ||
		    key.UnicodeChar == CHAR_LINEFEED ||
		    key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
			continue;
		}

		if (count == 0 && key.UnicodeChar == CHAR_BACKSPACE) {
			continue;
		} else if (key.UnicodeChar == CHAR_BACKSPACE) {
			if (show) {
				Print(L"\b");
			}
			line[--count] = '\0';
			continue;
		}

		if (show) {
			Print(L"%c", key.UnicodeChar);
		}

		line[count++] = key.UnicodeChar;
	} while (key.UnicodeChar != CHAR_CARRIAGE_RETURN);
	Print(L"\n");

	*length = count;

	return 1;
}

static EFI_STATUS compute_pw_hash (void *Data, UINTN DataSize, UINT8 *password,
				   UINT32 pw_length, UINT8 *hash)
{
	EFI_STATUS status;
	unsigned int ctxsize;
	void *ctx = NULL;

	ctxsize = Sha256GetContextSize();
	ctx = AllocatePool(ctxsize);

	if (!ctx) {
		console_notify(L"Unable to allocate memory for hash context");
		return EFI_OUT_OF_RESOURCES;
	}

	if (!Sha256Init(ctx)) {
		console_notify(L"Unable to initialise hash");
		status = EFI_OUT_OF_RESOURCES;
		goto done;
	}

	if (Data && DataSize) {
		if (!(Sha256Update(ctx, Data, DataSize))) {
			console_notify(L"Unable to generate hash");
			status = EFI_OUT_OF_RESOURCES;
			goto done;
		}
	}

	if (!(Sha256Update(ctx, password, pw_length))) {
		console_notify(L"Unable to generate hash");
		status = EFI_OUT_OF_RESOURCES;
		goto done;
	}

	if (!(Sha256Final(ctx, hash))) {
		console_notify(L"Unable to finalise hash");
		status = EFI_OUT_OF_RESOURCES;
		goto done;
	}

	status = EFI_SUCCESS;
done:
	return status;
}

static EFI_STATUS match_password (PASSWORD_CRYPT *pw_crypt,
				  void *Data, UINTN DataSize,
				  UINT8 *auth, CHAR16 *prompt)
{
	EFI_STATUS status;
	UINT8 hash[128];
	UINT8 *auth_hash;
	UINT32 auth_size;
	CHAR16 password[PASSWORD_MAX];
	UINT32 pw_length;
	UINT8 fail_count = 0;
	int i;

	if (pw_crypt) {
		auth_hash = pw_crypt->hash;
		auth_size = get_hash_size (pw_crypt->method);
		if (auth_size == 0)
			return EFI_INVALID_PARAMETER;
	} else if (auth) {
		auth_hash = auth;
		auth_size = SHA256_DIGEST_SIZE;
	} else {
		return EFI_INVALID_PARAMETER;
	}

	while (fail_count < 3) {
		if (prompt) {
			Print(L"%s", prompt);
		} else {
			Print(L"Password: ");
		}
		get_line(&pw_length, password, PASSWORD_MAX, 0);

		if (pw_length < PASSWORD_MIN || pw_length > PASSWORD_MAX) {
			Print(L"Invalid password length\n");
			fail_count++;
			continue;
		}

		/*
		 * Compute password hash
		 */
		if (pw_crypt) {
			char pw_ascii[PASSWORD_MAX + 1];
			for (i = 0; i < pw_length; i++)
				pw_ascii[i] = (char)password[i];
			pw_ascii[pw_length] = '\0';

			status = password_crypt(pw_ascii, pw_length, pw_crypt, hash);
		} else {
			/*
			 * For backward compatibility
			 */
			status = compute_pw_hash(Data, DataSize, (UINT8 *)password,
						 pw_length * sizeof(CHAR16), hash);
		}
		if (status != EFI_SUCCESS) {
			Print(L"Unable to generate password hash\n");
			fail_count++;
			continue;
		}

		if (CompareMem(auth_hash, hash, auth_size) != 0) {
			Print(L"Password doesn't match\n");
			fail_count++;
			continue;
		}

		break;
	}

	if (fail_count >= 3)
		return EFI_ACCESS_DENIED;

	return EFI_SUCCESS;
}

static EFI_STATUS store_keys (void *MokNew, UINTN MokNewSize, int authenticate)
{
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;
	UINT8 auth[PASSWORD_CRYPT_SIZE];
	UINTN auth_size = PASSWORD_CRYPT_SIZE;
	UINT32 attributes;

	if (authenticate) {
		efi_status = uefi_call_wrapper(RT->GetVariable, 5, L"MokAuth",
					       &shim_lock_guid,
					       &attributes, &auth_size, auth);

		if (efi_status != EFI_SUCCESS ||
		    (auth_size != SHA256_DIGEST_SIZE &&
		     auth_size != PASSWORD_CRYPT_SIZE)) {
			console_error(L"Failed to get MokAuth", efi_status);
			return efi_status;
		}

		if (auth_size == PASSWORD_CRYPT_SIZE) {
			efi_status = match_password((PASSWORD_CRYPT *)auth,
						    NULL, 0, NULL, NULL);
		} else {
			efi_status = match_password(NULL, MokNew, MokNewSize,
						    auth, NULL);
		}
		if (efi_status != EFI_SUCCESS)
			return EFI_ACCESS_DENIED;
	}

	if (!MokNewSize) {
		/* Delete MOK */
		efi_status = uefi_call_wrapper(RT->SetVariable, 5, L"MokList",
					       &shim_lock_guid,
					       EFI_VARIABLE_NON_VOLATILE
					       | EFI_VARIABLE_BOOTSERVICE_ACCESS,
					       0, NULL);
	} else {
		/* Write new MOK */
		efi_status = uefi_call_wrapper(RT->SetVariable, 5, L"MokList",
					       &shim_lock_guid,
					       EFI_VARIABLE_NON_VOLATILE
					       | EFI_VARIABLE_BOOTSERVICE_ACCESS
					       | EFI_VARIABLE_APPEND_WRITE,
					       MokNewSize, MokNew);
	}

	if (efi_status != EFI_SUCCESS) {
		console_error(L"Failed to set variable", efi_status);
		return efi_status;
	}

	return EFI_SUCCESS;
}

static UINTN mok_enrollment_prompt (void *MokNew, UINTN MokNewSize, int auth) {
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;

	if (list_keys(MokNew, MokNewSize, L"[Enroll MOK]") != EFI_SUCCESS)
		return 0;

	if (console_yes_no((CHAR16 *[]){L"Enroll the key(s)?", NULL}) == 0)
		return 0;

	efi_status = store_keys(MokNew, MokNewSize, auth);

	if (efi_status != EFI_SUCCESS) {
		console_notify(L"Failed to enroll keys\n");
		return -1;
	}

	if (auth) {
		LibDeleteVariable(L"MokNew", &shim_lock_guid);
		LibDeleteVariable(L"MokAuth", &shim_lock_guid);

		console_notify(L"The system must now be rebooted");
		uefi_call_wrapper(RT->ResetSystem, 4, EfiResetWarm,
				  EFI_SUCCESS, 0, NULL);
		console_notify(L"Failed to reboot");
		return -1;
	}

	return 0;
}

static INTN mok_reset_prompt ()
{
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;

	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

	if (console_yes_no((CHAR16 *[]){L"Erase all stored keys?", NULL }) == 0)
		return 0;

	efi_status = store_keys(NULL, 0, TRUE);

	if (efi_status != EFI_SUCCESS) {
		console_notify(L"Failed to erase keys\n");
		return -1;
	}

	LibDeleteVariable(L"MokNew", &shim_lock_guid);
	LibDeleteVariable(L"MokAuth", &shim_lock_guid);

	console_notify(L"The system must now be rebooted");
	uefi_call_wrapper(RT->ResetSystem, 4, EfiResetWarm,
			  EFI_SUCCESS, 0, NULL);
	console_notify(L"Failed to reboot\n");
	return -1;
}

static EFI_STATUS write_back_mok_list (MokListNode *list, INTN key_num)
{
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;
	EFI_SIGNATURE_LIST *CertList;
	EFI_SIGNATURE_DATA *CertData;
	void *Data = NULL, *ptr;
	INTN DataSize = 0;
	int i;

	for (i = 0; i < key_num; i++) {
		if (list[i].Mok == NULL)
			continue;

		DataSize += sizeof(EFI_SIGNATURE_LIST) + sizeof(EFI_GUID);
		DataSize += list[i].MokSize;
	}

	Data = AllocatePool(DataSize);
	if (Data == NULL && DataSize != 0)
		return EFI_OUT_OF_RESOURCES;

	ptr = Data;

	for (i = 0; i < key_num; i++) {
		if (list[i].Mok == NULL)
			continue;

		CertList = (EFI_SIGNATURE_LIST *)ptr;
		CertData = (EFI_SIGNATURE_DATA *)(((uint8_t *)ptr) +
			   sizeof(EFI_SIGNATURE_LIST));

		CertList->SignatureType = list[i].Type;
		CertList->SignatureListSize = list[i].MokSize +
					      sizeof(EFI_SIGNATURE_LIST) +
					      sizeof(EFI_SIGNATURE_DATA) - 1;
		CertList->SignatureHeaderSize = 0;
		CertList->SignatureSize = list[i].MokSize + sizeof(EFI_GUID);

		CertData->SignatureOwner = shim_lock_guid;
		CopyMem(CertData->SignatureData, list[i].Mok, list[i].MokSize);

		ptr = (uint8_t *)ptr + sizeof(EFI_SIGNATURE_LIST) +
		      sizeof(EFI_GUID) + list[i].MokSize;
	}

	efi_status = uefi_call_wrapper(RT->SetVariable, 5, L"MokList",
				       &shim_lock_guid,
				       EFI_VARIABLE_NON_VOLATILE
				       | EFI_VARIABLE_BOOTSERVICE_ACCESS,
				       DataSize, Data);
	if (Data)
		FreePool(Data);

	if (efi_status != EFI_SUCCESS) {
		console_error(L"Failed to set variable", efi_status);
		return efi_status;
	}

	return EFI_SUCCESS;
}

static EFI_STATUS delete_keys (void *MokDel, UINTN MokDelSize)
{
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;
	UINT8 auth[PASSWORD_CRYPT_SIZE];
	UINTN auth_size = PASSWORD_CRYPT_SIZE;
	UINT32 attributes;
	void *MokListData = NULL;
	UINTN MokListDataSize = 0;
	MokListNode *mok, *del_key;
	INTN mok_num, del_num;
	int i, j;

	efi_status = uefi_call_wrapper(RT->GetVariable, 5, L"MokDelAuth",
				       &shim_lock_guid,
				       &attributes, &auth_size, auth);

	if (efi_status != EFI_SUCCESS ||
	    (auth_size != SHA256_DIGEST_SIZE && auth_size != PASSWORD_CRYPT_SIZE)) {
		console_error(L"Failed to get MokDelAuth", efi_status);
		return efi_status;
	}

	if (auth_size == PASSWORD_CRYPT_SIZE) {
		efi_status = match_password((PASSWORD_CRYPT *)auth, NULL, 0,
					    NULL, NULL);
	} else {
		efi_status = match_password(NULL, MokDel, MokDelSize, auth, NULL);
	}
	if (efi_status != EFI_SUCCESS)
		return EFI_ACCESS_DENIED;

	efi_status = get_variable(L"MokList", shim_lock_guid, &attributes,
				  &MokListDataSize, &MokListData);

	if (attributes & EFI_VARIABLE_RUNTIME_ACCESS) {
		console_alertbox((CHAR16 *[]){L"MokList is compromised!",
					L"Erase all keys in MokList!",
					NULL});
		if (LibDeleteVariable(L"MokList", &shim_lock_guid) != EFI_SUCCESS) {
			console_notify(L"Failed to erase MokList");
		}
		return EFI_ACCESS_DENIED;
	}

	/* Nothing to do */
	if (!MokListData || MokListDataSize == 0)
		return EFI_SUCCESS;

	/* Construct lists */
	mok_num = count_keys(MokListData, MokListDataSize);
	mok = build_mok_list(mok_num, MokListData, MokListDataSize);
	del_num = count_keys(MokDel, MokDelSize);
	del_key = build_mok_list(del_num, MokDel, MokDelSize);

	/* Search and destroy */
	for (i = 0; i < del_num; i++) {
		UINT32 key_size = del_key[i].MokSize;
		void *key = del_key[i].Mok;
		for (j = 0; j < mok_num; j++) {
			if (mok[j].MokSize == key_size &&
			    CompareMem(key, mok[j].Mok, key_size) == 0) {
				/* Remove the key */
				mok[j].Mok = NULL;
				mok[j].MokSize = 0;
			}
		}
	}

	efi_status = write_back_mok_list(mok, mok_num);

	if (MokListData)
		FreePool(MokListData);
	if (mok)
		FreePool(mok);
	if (del_key)
		FreePool(del_key);

	return efi_status;
}

static INTN mok_deletion_prompt (void *MokDel, UINTN MokDelSize)
{
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;

	if (list_keys(MokDel, MokDelSize, L"[Delete MOK]") != EFI_SUCCESS) {
		return 0;
	}

        if (console_yes_no((CHAR16 *[]){L"Delete the key(s)?", NULL}) == 0)
                return 0;

	efi_status = delete_keys(MokDel, MokDelSize);

	if (efi_status != EFI_SUCCESS) {
		console_notify(L"Failed to delete keys");
		return -1;
	}

	LibDeleteVariable(L"MokDel", &shim_lock_guid);
	LibDeleteVariable(L"MokDelAuth", &shim_lock_guid);

	console_notify(L"The system must now be rebooted");
	uefi_call_wrapper(RT->ResetSystem, 4, EfiResetWarm,
			  EFI_SUCCESS, 0, NULL);
	console_notify(L"Failed to reboot");
	return -1;
}

static INTN mok_sb_prompt (void *MokSB, UINTN MokSBSize) {
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;
	MokSBvar *var = MokSB;
	CHAR16 pass1, pass2, pass3;
	UINT8 fail_count = 0;
	UINT32 length;
	UINT8 sbval = 1;
	UINT8 pos1, pos2, pos3;
	int ret;

	if (MokSBSize != sizeof(MokSBvar)) {
		console_notify(L"Invalid MokSB variable contents");
		return -1;
	}

	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

	while (fail_count < 3) {
		RandomBytes (&pos1, sizeof(pos1));
		pos1 = (pos1 % var->PWLen);

		do {
			RandomBytes (&pos2, sizeof(pos2));
			pos2 = (pos2 % var->PWLen);
		} while (pos2 == pos1);

		do {
			RandomBytes (&pos3, sizeof(pos3));
			pos3 = (pos3 % var->PWLen) ;
		} while (pos3 == pos2 || pos3 == pos1);

		Print(L"Enter password character %d: ", pos1 + 1);
		get_line(&length, &pass1, 1, 0);

		Print(L"Enter password character %d: ", pos2 + 1);
		get_line(&length, &pass2, 1, 0);

		Print(L"Enter password character %d: ", pos3 + 1);
		get_line(&length, &pass3, 1, 0);

		if (pass1 != var->Password[pos1] ||
		    pass2 != var->Password[pos2] ||
		    pass3 != var->Password[pos3]) {
			Print(L"Invalid character\n");
			fail_count++;
		} else {
			break;
		}
	}

	if (fail_count >= 3) {
		console_notify(L"Password limit reached");
		return -1;
	}

	if (var->MokSBState == 0)
		ret = console_yes_no((CHAR16 *[]){L"Disable Secure Boot", NULL});
	else
		ret = console_yes_no((CHAR16 *[]){L"Enable Secure Boot", NULL});

	if (ret == 0) {
		LibDeleteVariable(L"MokSB", &shim_lock_guid);
		return -1;
	}

	if (var->MokSBState == 0) {
		efi_status = uefi_call_wrapper(RT->SetVariable,
					       5, L"MokSBState",
					       &shim_lock_guid,
					       EFI_VARIABLE_NON_VOLATILE |
					       EFI_VARIABLE_BOOTSERVICE_ACCESS,
					       1, &sbval);
		if (efi_status != EFI_SUCCESS) {
			console_notify(L"Failed to set Secure Boot state");
			return -1;
		}
	} else {
		LibDeleteVariable(L"MokSBState", &shim_lock_guid);
	}

	console_notify(L"The system must now be rebooted");
	uefi_call_wrapper(RT->ResetSystem, 4, EfiResetWarm,
			  EFI_SUCCESS, 0, NULL);
	console_notify(L"Failed to reboot");
	return -1;
}

static INTN mok_pw_prompt (void *MokPW, UINTN MokPWSize) {
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;
	UINT8 hash[PASSWORD_CRYPT_SIZE];
	UINT8 clear = 0;

	if (MokPWSize != SHA256_DIGEST_SIZE && MokPWSize != PASSWORD_CRYPT_SIZE) {
		console_notify(L"Invalid MokPW variable contents");
		return -1;
	}

	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

	SetMem(hash, PASSWORD_CRYPT_SIZE, 0);

	if (MokPWSize == PASSWORD_CRYPT_SIZE) {
		if (CompareMem(MokPW, hash, PASSWORD_CRYPT_SIZE) == 0)
			clear = 1;
	} else {
		if (CompareMem(MokPW, hash, SHA256_DIGEST_SIZE) == 0)
			clear = 1;
	}

	if (clear) {
		if (console_yes_no((CHAR16 *[]){L"Clear MOK password?", NULL}) == 0)
			return 0;

		LibDeleteVariable(L"MokPWStore", &shim_lock_guid);
		LibDeleteVariable(L"MokPW", &shim_lock_guid);
		return 0;
	}

	if (MokPWSize == PASSWORD_CRYPT_SIZE) {
		efi_status = match_password((PASSWORD_CRYPT *)MokPW, NULL, 0,
					    NULL, L"Confirm MOK passphrase: ");
	} else {
		efi_status = match_password(NULL, NULL, 0, MokPW,
					    L"Confirm MOK passphrase: ");
	}

	if (efi_status != EFI_SUCCESS) {
		console_notify(L"Password limit reached");
		return -1;
	}

	if (console_yes_no((CHAR16 *[]){L"Set MOK password?", NULL}) == 0)
		return 0;

	efi_status = uefi_call_wrapper(RT->SetVariable, 5,
				       L"MokPWStore",
				       &shim_lock_guid,
				       EFI_VARIABLE_NON_VOLATILE |
				       EFI_VARIABLE_BOOTSERVICE_ACCESS,
				       MokPWSize, MokPW);
	if (efi_status != EFI_SUCCESS) {
		console_notify(L"Failed to set MOK password");
		return -1;
	}

	LibDeleteVariable(L"MokPW", &shim_lock_guid);

	console_notify(L"The system must now be rebooted");
	uefi_call_wrapper(RT->ResetSystem, 4, EfiResetWarm, EFI_SUCCESS, 0,
			  NULL);
	console_notify(L"Failed to reboot");
	return -1;
}

static UINTN verify_certificate(void *cert, UINTN size)
{
	X509 *X509Cert;
	if (!cert || size == 0)
		return FALSE;

	if (!(X509ConstructCertificate(cert, size, (UINT8 **) &X509Cert)) ||
	    X509Cert == NULL) {
		console_notify(L"Invalid X509 certificate");
		return FALSE;
	}

	X509_free(X509Cert);
	return TRUE;
}

static EFI_STATUS enroll_file (void *data, UINTN datasize, BOOLEAN hash)
{
	EFI_STATUS status = EFI_SUCCESS;
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_SIGNATURE_LIST *CertList;
	EFI_SIGNATURE_DATA *CertData;
	UINTN mokbuffersize;
	void *mokbuffer = NULL;

	if (hash) {
		UINT8 sha256[SHA256_DIGEST_SIZE];
		UINT8 sha1[SHA1_DIGEST_SIZE];
		SHIM_LOCK *shim_lock;
		EFI_GUID shim_guid = SHIM_LOCK_GUID;
		PE_COFF_LOADER_IMAGE_CONTEXT context;
	
		status = LibLocateProtocol(&shim_guid, (VOID **)&shim_lock);

		if (status != EFI_SUCCESS)
			goto out;

		mokbuffersize = sizeof(EFI_SIGNATURE_LIST) + sizeof(EFI_GUID) +
			SHA256_DIGEST_SIZE;

		mokbuffer = AllocatePool(mokbuffersize);

		if (!mokbuffer)
			goto out;

		status = shim_lock->Context(data, datasize, &context);

		if (status != EFI_SUCCESS)
			goto out;
	
		status = shim_lock->Hash(data, datasize, &context, sha256,
					 sha1);

		if (status != EFI_SUCCESS)
			goto out;

		CertList = mokbuffer;
		CertList->SignatureType = EfiHashSha256Guid;
		CertList->SignatureSize = 16 + SHA256_DIGEST_SIZE;
		CertData = (EFI_SIGNATURE_DATA *)(((UINT8 *)mokbuffer) +
						  sizeof(EFI_SIGNATURE_LIST));
		CopyMem(CertData->SignatureData, sha256, SHA256_DIGEST_SIZE);
	} else {
		mokbuffersize = datasize + sizeof(EFI_SIGNATURE_LIST) +
			sizeof(EFI_GUID);
		mokbuffer = AllocatePool(mokbuffersize);

		if (!mokbuffer)
			goto out;

		CertList = mokbuffer;
		CertList->SignatureType = EfiCertX509Guid;
		CertList->SignatureSize = 16 + datasize;

		memcpy(mokbuffer + sizeof(EFI_SIGNATURE_LIST) + 16, data,
		       datasize);

		CertData = (EFI_SIGNATURE_DATA *)(((UINT8 *)mokbuffer) +
						  sizeof(EFI_SIGNATURE_LIST));
	}

	CertList->SignatureListSize = mokbuffersize;
	CertList->SignatureHeaderSize = 0;
	CertData->SignatureOwner = shim_lock_guid;

	if (!hash) {
		if (!verify_certificate(CertData->SignatureData, datasize))
			goto out;
	}

	mok_enrollment_prompt(mokbuffer, mokbuffersize, FALSE);
out:
	if (mokbuffer)
		FreePool(mokbuffer);

	return status;
}

static void mok_hash_enroll(void)
{
	EFI_STATUS efi_status;
        CHAR16 *file_name = NULL;
	EFI_HANDLE im = NULL;
	EFI_FILE *file = NULL;
	UINTN filesize;
	void *data;

	simple_file_selector(&im, (CHAR16 *[]){
      L"Select Binary",
      L"",
      L"The Selected Binary will have its hash Enrolled",
      L"This means it will Subsequently Boot with no prompting",
      L"Remember to make sure it is a genuine binary before Enroling its hash",
      NULL
	      }, L"\\", L"", &file_name);

	if (!file_name)
		return;

	efi_status = simple_file_open(im, file_name, &file, EFI_FILE_MODE_READ);

	if (efi_status != EFI_SUCCESS) {
		console_error(L"Unable to open file", efi_status);
		return;
	}

	simple_file_read_all(file, &filesize, &data);
	simple_file_close(file);

	if (!filesize) {
		console_error(L"Unable to read file", efi_status);
		return;
	}

	efi_status = enroll_file(data, filesize, TRUE);

	if (efi_status != EFI_SUCCESS)
		console_error(L"Hash failed (did you select a valid EFI binary?)", efi_status);

	FreePool(data);
}

static void mok_key_enroll(void)
{
	EFI_STATUS efi_status;
        CHAR16 *file_name = NULL;
	EFI_HANDLE im = NULL;
	EFI_FILE *file = NULL;
	UINTN filesize;
	void *data;

	simple_file_selector(&im, (CHAR16 *[]){
      L"Select Key",
      L"",
      L"The selected key will be enrolled into the MOK database",
      L"This means any binaries signed with it will be run without prompting",
      L"Remember to make sure it is a genuine key before Enroling it",
      NULL
	      }, L"\\", L"", &file_name);

	if (!file_name)
		return;

	efi_status = simple_file_open(im, file_name, &file, EFI_FILE_MODE_READ);

	if (efi_status != EFI_SUCCESS) {
		console_error(L"Unable to open file", efi_status);
		return;
	}

	simple_file_read_all(file, &filesize, &data);
	simple_file_close(file);

	if (!filesize) {
		console_error(L"Unable to read file", efi_status);
		return;
	}

	enroll_file(data, filesize, FALSE);
	FreePool(data);
}

static BOOLEAN verify_pw(void)
{
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	EFI_STATUS efi_status;
	UINT8 pwhash[PASSWORD_CRYPT_SIZE];
	UINTN size = PASSWORD_CRYPT_SIZE;
	UINT32 attributes;

	efi_status = uefi_call_wrapper(RT->GetVariable, 5, L"MokPWStore",
				       &shim_lock_guid, &attributes, &size,
				       pwhash);

	/*
	 * If anything can attack the password it could just set it to a
	 * known value, so there's no safety advantage in failing to validate
	 * purely because of a failure to read the variable
	 */
	if (efi_status != EFI_SUCCESS ||
	    (size != SHA256_DIGEST_SIZE && size != PASSWORD_CRYPT_SIZE))
		return TRUE;

	if (attributes & EFI_VARIABLE_RUNTIME_ACCESS)
		return TRUE;

	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

	if (size == PASSWORD_CRYPT_SIZE) {
		efi_status = match_password((PASSWORD_CRYPT *)pwhash, NULL, 0,
					    NULL, L"Enter MOK password: ");
	} else {
		efi_status = match_password(NULL, NULL, 0, pwhash,
					    L"Enter MOK password: ");
	}
	if (efi_status != EFI_SUCCESS) {
		console_notify(L"Password limit reached");
		return FALSE;
	}

	return TRUE;
}

typedef enum {
	MOK_CONTINUE_BOOT,
	MOK_RESET_MOK,
	MOK_ENROLL_MOK,
	MOK_DELETE_MOK,
	MOK_CHANGE_SB,
	MOK_SET_PW,
	MOK_KEY_ENROLL,
	MOK_HASH_ENROLL
} mok_menu_item;

static EFI_STATUS enter_mok_menu(EFI_HANDLE image_handle,
				 void *MokNew, UINTN MokNewSize,
				 void *MokDel, UINTN MokDelSize,
				 void *MokSB, UINTN MokSBSize,
				 void *MokPW, UINTN MokPWSize)
{
	CHAR16 **menu_strings;
	mok_menu_item *menu_item;
	int choice = 0;
	UINT32 MokAuth = 0;
	UINT32 MokDelAuth = 0;
	UINTN menucount = 3, i = 0;
	EFI_STATUS efi_status;
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	UINT8 auth[PASSWORD_CRYPT_SIZE];
	UINTN auth_size = PASSWORD_CRYPT_SIZE;
	UINT32 attributes;
	EFI_STATUS ret = EFI_SUCCESS;

	if (verify_pw() == FALSE)
		return EFI_ACCESS_DENIED;
	
	efi_status = uefi_call_wrapper(RT->GetVariable, 5, L"MokAuth",
					       &shim_lock_guid,
					       &attributes, &auth_size, auth);

	if ((efi_status == EFI_SUCCESS) &&
	    (auth_size == SHA256_DIGEST_SIZE || auth_size == PASSWORD_CRYPT_SIZE))
		MokAuth = 1;

	efi_status = uefi_call_wrapper(RT->GetVariable, 5, L"MokDelAuth",
					       &shim_lock_guid,
					       &attributes, &auth_size, auth);

	if ((efi_status == EFI_SUCCESS) &&
	   (auth_size == SHA256_DIGEST_SIZE || auth_size == PASSWORD_CRYPT_SIZE))
		MokDelAuth = 1;

	if (MokNew || MokAuth)
		menucount++;

	if (MokDel || MokDelAuth)
		menucount++;

	if (MokSB)
		menucount++;

	if (MokPW)
		menucount++;

	menu_strings = AllocateZeroPool(sizeof(CHAR16 *) * (menucount + 1));

	if (!menu_strings)
		return EFI_OUT_OF_RESOURCES;

	menu_item = AllocateZeroPool(sizeof(mok_menu_item) * menucount);

	if (!menu_item) {
		FreePool(menu_strings);
		return EFI_OUT_OF_RESOURCES;
	}

	menu_strings[i] = StrDuplicate(L"Continue boot");
	menu_item[i] = MOK_CONTINUE_BOOT;

	i++;

	if (MokNew || MokAuth) {
		if (!MokNew) {
			menu_strings[i] = StrDuplicate(L"Reset MOK");
			menu_item[i] = MOK_RESET_MOK;
		} else {
			menu_strings[i] = StrDuplicate(L"Enroll MOK");
			menu_item[i] = MOK_ENROLL_MOK;
		}
		i++;
	}

	if (MokDel || MokDelAuth) {		
		menu_strings[i] = StrDuplicate(L"Delete MOK");
		menu_item[i] = MOK_DELETE_MOK;
		i++;
	}

	if (MokSB) {
		menu_strings[i] = StrDuplicate(L"Change Secure Boot state");
		menu_item[i] = MOK_CHANGE_SB;
		i++;
	}

	if (MokPW) {
		menu_strings[i] = StrDuplicate(L"Set MOK password");
		menu_item[i] = MOK_SET_PW;
		i++;
	}

	menu_strings[i] = StrDuplicate(L"Enroll key from disk");
	menu_item[i] = MOK_KEY_ENROLL;
	i++;

	menu_strings[i] = StrDuplicate(L"Enroll hash from disk");
	menu_item[i] = MOK_HASH_ENROLL;
	i++;

	menu_strings[i] = NULL;

	while (choice >= 0) {
		choice = console_select((CHAR16 *[]){ L"Perform MOK management", NULL },
					menu_strings, 0);

		if (choice < 0)
			goto out;

		switch (menu_item[choice]) {
		case MOK_CONTINUE_BOOT:
			goto out;
		case MOK_RESET_MOK:
			mok_reset_prompt();
			break;
		case MOK_ENROLL_MOK:
			mok_enrollment_prompt(MokNew, MokNewSize, TRUE);
			break;
		case MOK_DELETE_MOK:
			mok_deletion_prompt(MokDel, MokDelSize);
			break;
		case MOK_CHANGE_SB:
			mok_sb_prompt(MokSB, MokSBSize);
			break;
		case MOK_SET_PW:
			mok_pw_prompt(MokPW, MokPWSize);
			break;
		case MOK_KEY_ENROLL:
			mok_key_enroll();
			break;
		case MOK_HASH_ENROLL:
			mok_hash_enroll();
			break;
		}
	}

out:
	console_reset();

	for (i=0; menu_strings[i] != NULL; i++)
		FreePool(menu_strings[i]);

	FreePool(menu_strings);

	if (menu_item)
		FreePool(menu_item);

	return ret;
}

static EFI_STATUS check_mok_request(EFI_HANDLE image_handle)
{
	EFI_GUID shim_lock_guid = SHIM_LOCK_GUID;
	UINTN MokNewSize = 0, MokDelSize = 0, MokSBSize = 0, MokPWSize = 0;
	void *MokNew = NULL;
	void *MokDel = NULL;
	void *MokSB = NULL;
	void *MokPW = NULL;

	MokNew = LibGetVariableAndSize(L"MokNew", &shim_lock_guid, &MokNewSize);

	MokDel = LibGetVariableAndSize(L"MokDel", &shim_lock_guid, &MokDelSize);

	MokSB = LibGetVariableAndSize(L"MokSB", &shim_lock_guid, &MokSBSize);

	MokPW = LibGetVariableAndSize(L"MokPW", &shim_lock_guid, &MokPWSize);

	enter_mok_menu(image_handle, MokNew, MokNewSize, MokDel, MokDelSize,
		       MokSB, MokSBSize, MokPW, MokPWSize);

	if (MokNew) {
		if (LibDeleteVariable(L"MokNew", &shim_lock_guid) != EFI_SUCCESS) {
			console_notify(L"Failed to delete MokNew");
		}
		FreePool (MokNew);
	}

	if (MokDel) {
		if (LibDeleteVariable(L"MokDel", &shim_lock_guid) != EFI_SUCCESS) {
			console_notify(L"Failed to delete MokDel");
		}
		FreePool (MokDel);
	}

	if (MokSB) {
		if (LibDeleteVariable(L"MokSB", &shim_lock_guid) != EFI_SUCCESS) {
			console_notify(L"Failed to delete MokSB");
		}
		FreePool (MokNew);
	}

	if (MokPW) {
		if (LibDeleteVariable(L"MokPW", &shim_lock_guid) != EFI_SUCCESS) {
			console_notify(L"Failed to delete MokPW");
		}
		FreePool (MokNew);
	}

	LibDeleteVariable(L"MokAuth", &shim_lock_guid);
	LibDeleteVariable(L"MokDelAuth", &shim_lock_guid);

	return EFI_SUCCESS;
}

static EFI_STATUS setup_rand (void)
{
	EFI_TIME time;
	EFI_STATUS efi_status;
	UINT64 seed;
	BOOLEAN status;

	efi_status = uefi_call_wrapper(RT->GetTime, 2, &time, NULL);

	if (efi_status != EFI_SUCCESS)
		return efi_status;

	seed = ((UINT64)time.Year << 48) | ((UINT64)time.Month << 40) |
		((UINT64)time.Day << 32) | ((UINT64)time.Hour << 24) |
		((UINT64)time.Minute << 16) | ((UINT64)time.Second << 8) |
		((UINT64)time.Daylight);

	status = RandomSeed((UINT8 *)&seed, sizeof(seed));

	if (!status)
		return EFI_ABORTED;

	return EFI_SUCCESS;
}

EFI_STATUS efi_main (EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *systab)
{
	EFI_STATUS efi_status;

	InitializeLib(image_handle, systab);

	setup_rand();

	efi_status = check_mok_request(image_handle);

	return efi_status;
}
