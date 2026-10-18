#include "WindowsUpdateTrust.h"
#include <QCryptographicHash>
#include <QDir>
#ifdef Q_OS_WIN
#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <string>
#endif

namespace {
// The trust anchors of Windows self-update. Both stay empty until the owner creates
// the update key in the release environment and chooses the signing provider
// (zerus-qxdx.1); setting them is a reviewed source change, never a build option.
const char manifestKeyBase64[]="";
const char publisher[]="";
}

QByteArray WindowsUpdateTrust::manifestKey(){
    const auto key=QByteArray::fromBase64Encoding(QByteArray(manifestKeyBase64),QByteArray::AbortOnBase64DecodingErrors);
    return key&&key.decoded.size()==64?key.decoded:QByteArray();
}
QString WindowsUpdateTrust::expectedPublisher(){return QString::fromUtf8(publisher);}
bool WindowsUpdateTrust::selfUpdateConfigured(){
#ifdef Q_OS_WIN
    return !manifestKey().isEmpty()&&!expectedPublisher().isEmpty();
#else
    return false;
#endif
}

bool WindowsUpdateTrust::verifyManifest(const QByteArray &data,const QByteArray &signature,const QByteArray &publicKey){
    if(signature.size()!=64||publicKey.size()!=64)return false;
#ifdef Q_OS_WIN
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_ECDSA_P256_ALGORITHM,nullptr,0)))return false;
    // BCRYPT_ECCPUBLIC_BLOB: the header, then X and Y.
    QByteArray blob(sizeof(BCRYPT_ECCKEY_BLOB),'\0');
    auto *header=reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob.data());
    header->dwMagic=BCRYPT_ECDSA_PUBLIC_P256_MAGIC;header->cbKey=32;
    blob+=publicKey;
    BCRYPT_KEY_HANDLE key=nullptr;bool valid=false;
    if(BCRYPT_SUCCESS(BCryptImportKeyPair(algorithm,nullptr,BCRYPT_ECCPUBLIC_BLOB,&key,reinterpret_cast<PUCHAR>(blob.data()),ULONG(blob.size()),0))){
        QByteArray digest=QCryptographicHash::hash(data,QCryptographicHash::Sha256),copy=signature;
        // STATUS_SUCCESS; an invalid signature is STATUS_INVALID_SIGNATURE.
        valid=BCryptVerifySignature(key,nullptr,reinterpret_cast<PUCHAR>(digest.data()),ULONG(digest.size()),
                                    reinterpret_cast<PUCHAR>(copy.data()),ULONG(copy.size()),0)==0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(algorithm,0);
    return valid;
#else
    Q_UNUSED(data);
    return false;
#endif
}

QString WindowsUpdateTrust::authenticodePublisher(const QString &path){
#ifdef Q_OS_WIN
    const std::wstring file=QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO info{};
    info.cbStruct=sizeof(info);info.pcwszFilePath=file.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_WHOLECHAIN;trust.dwProvFlags=WTD_REVOCATION_CHECK_CHAIN;
    trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&info;trust.dwStateAction=WTD_STATEACTION_VERIFY;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    QString name;
    if(WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),&action,&trust)==ERROR_SUCCESS){
        CRYPT_PROVIDER_DATA *provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
        CRYPT_PROVIDER_SGNR *signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        CRYPT_PROVIDER_CERT *certificate=signer?WTHelperGetProvCertFromChain(signer,0):nullptr;
        if(certificate&&certificate->pCert){
            wchar_t buffer[256]{};
            if(CertGetNameStringW(certificate->pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,buffer,256)>1)name=QString::fromWCharArray(buffer);
        }
    }
    // Releases the state of the verification, whatever its result.
    trust.dwStateAction=WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),&action,&trust);
    return name;
#else
    Q_UNUSED(path);
    return {};
#endif
}
