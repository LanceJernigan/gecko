/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsReadableUtils.h"
#include "nsUnicharUtils.h"
#include "nsCharsetAlias.h"
#include "nsIServiceManager.h"
#include "nsICategoryManager.h"
#include "nsICharsetConverterManager.h"
#include "nsEncoderDecoderUtils.h"
#include "nsIStringBundle.h"
#include "prmem.h"
#include "nsCRT.h"
#include "nsTArray.h"
#include "nsStringEnumerator.h"
#include "nsThreadUtils.h"
#include "mozilla/Services.h"

#include "nsXPCOM.h"
#include "nsComponentManagerUtils.h"
#include "nsISupportsPrimitives.h"

// just for CONTRACTIDs
#include "nsCharsetConverterManager.h"

// Class nsCharsetConverterManager [implementation]

NS_IMPL_THREADSAFE_ISUPPORTS1(nsCharsetConverterManager,
                              nsICharsetConverterManager)

nsCharsetConverterManager::nsCharsetConverterManager() 
  : mDataBundle(NULL)
  , mTitleBundle(NULL)
{
}

nsCharsetConverterManager::~nsCharsetConverterManager() 
{
  NS_IF_RELEASE(mDataBundle);
  NS_IF_RELEASE(mTitleBundle);
}

nsresult nsCharsetConverterManager::LoadExtensibleBundle(
                                    const char* aCategory, 
                                    nsIStringBundle ** aResult)
{
  nsCOMPtr<nsIStringBundleService> sbServ =
    mozilla::services::GetStringBundleService();
  if (!sbServ)
    return NS_ERROR_FAILURE;

  return sbServ->CreateExtensibleBundle(aCategory, aResult);
}

nsresult nsCharsetConverterManager::GetBundleValue(nsIStringBundle * aBundle, 
                                                   const char * aName, 
                                                   const nsAFlatString& aProp, 
                                                   PRUnichar ** aResult)
{
  nsAutoString key; 

  key.AssignWithConversion(aName);
  ToLowerCase(key); // we lowercase the main comparison key
  key.Append(aProp);

  return aBundle->GetStringFromName(key.get(), aResult);
}

nsresult nsCharsetConverterManager::GetBundleValue(nsIStringBundle * aBundle, 
                                                   const char * aName, 
                                                   const nsAFlatString& aProp, 
                                                   nsAString& aResult)
{
  nsresult rv = NS_OK;

  nsXPIDLString value;
  rv = GetBundleValue(aBundle, aName, aProp, getter_Copies(value));
  if (NS_FAILED(rv))
    return rv;

  aResult = value;

  return NS_OK;
}


//----------------------------------------------------------------------------//----------------------------------------------------------------------------
// Interface nsICharsetConverterManager [implementation]

NS_IMETHODIMP
nsCharsetConverterManager::GetUnicodeEncoder(const char * aDest, 
                                             nsIUnicodeEncoder ** aResult)
{
  // resolve the charset first
  nsCAutoString charset;
  
  // fully qualify to possibly avoid vtable call
  nsCharsetConverterManager::GetCharsetAlias(aDest, charset);

  return nsCharsetConverterManager::GetUnicodeEncoderRaw(charset.get(),
                                                         aResult);
}


NS_IMETHODIMP
nsCharsetConverterManager::GetUnicodeEncoderRaw(const char * aDest, 
                                                nsIUnicodeEncoder ** aResult)
{
  *aResult= nsnull;
  nsCOMPtr<nsIUnicodeEncoder> encoder;

  nsresult rv = NS_OK;

  nsCAutoString
    contractid(NS_LITERAL_CSTRING(NS_UNICODEENCODER_CONTRACTID_BASE) +
               nsDependentCString(aDest));

  // Always create an instance since encoders hold state.
  encoder = do_CreateInstance(contractid.get(), &rv);

  if (NS_FAILED(rv))
    rv = NS_ERROR_UCONV_NOCONV;
  else
  {
    *aResult = encoder.get();
    NS_ADDREF(*aResult);
  }
  return rv;
}

NS_IMETHODIMP
nsCharsetConverterManager::GetUnicodeDecoderRaw(const char * aSrc,
                                                nsIUnicodeDecoder ** aResult)
{
  nsresult rv;

  nsAutoString str;
  rv = GetCharsetData(aSrc, NS_LITERAL_STRING(".isXSSVulnerable").get(), str);
  if (NS_SUCCEEDED(rv))
    return NS_ERROR_UCONV_NOCONV;

  return GetUnicodeDecoderRawInternal(aSrc, aResult);
}

NS_IMETHODIMP
nsCharsetConverterManager::GetUnicodeDecoder(const char * aSrc, 
                                             nsIUnicodeDecoder ** aResult)
{
  // resolve the charset first
  nsCAutoString charset;
  
  // fully qualify to possibly avoid vtable call
  nsCharsetConverterManager::GetCharsetAlias(aSrc, charset);

  return nsCharsetConverterManager::GetUnicodeDecoderRaw(charset.get(),
                                                         aResult);
}

NS_IMETHODIMP
nsCharsetConverterManager::GetUnicodeDecoderInternal(const char * aSrc, 
                                                     nsIUnicodeDecoder ** aResult)
{
  // resolve the charset first
  nsCAutoString charset;
  
  // fully qualify to possibly avoid vtable call
  nsresult rv = nsCharsetConverterManager::GetCharsetAlias(aSrc, charset);
  NS_ENSURE_SUCCESS(rv, rv);

  return nsCharsetConverterManager::GetUnicodeDecoderRawInternal(charset.get(),
                                                                 aResult);
}

NS_IMETHODIMP
nsCharsetConverterManager::GetUnicodeDecoderRawInternal(const char * aSrc, 
                                                        nsIUnicodeDecoder ** aResult)
{
  *aResult= nsnull;
  nsCOMPtr<nsIUnicodeDecoder> decoder;

  nsresult rv = NS_OK;

  NS_NAMED_LITERAL_CSTRING(contractbase, NS_UNICODEDECODER_CONTRACTID_BASE);
  nsDependentCString src(aSrc);
  
  decoder = do_CreateInstance(PromiseFlatCString(contractbase + src).get(),
                              &rv);
  NS_ENSURE_SUCCESS(rv, NS_ERROR_UCONV_NOCONV);

  decoder.forget(aResult);
  return rv;
}

nsresult 
nsCharsetConverterManager::GetList(const nsACString& aCategory,
                                   const nsACString& aPrefix,
                                   nsIUTF8StringEnumerator** aResult)
{
  if (aResult == NULL) 
    return NS_ERROR_NULL_POINTER;
  *aResult = NULL;

  nsresult rv;

  nsCOMPtr<nsICategoryManager> catman = do_GetService(NS_CATEGORYMANAGER_CONTRACTID, &rv);
  if (NS_FAILED(rv))
    return rv;

  nsTArray<nsCString>* array = new nsTArray<nsCString>;
  if (!array)
    return NS_ERROR_OUT_OF_MEMORY;
  
  nsCOMPtr<nsISimpleEnumerator> enumerator;
  catman->EnumerateCategory(PromiseFlatCString(aCategory).get(), 
                            getter_AddRefs(enumerator));

  bool hasMore;
  while (NS_SUCCEEDED(enumerator->HasMoreElements(&hasMore)) && hasMore) {
    nsCOMPtr<nsISupports> supports;
    if (NS_FAILED(enumerator->GetNext(getter_AddRefs(supports))))
      continue;
    
    nsCOMPtr<nsISupportsCString> supStr = do_QueryInterface(supports);
    if (!supStr)
      continue;

    nsCAutoString name;
    if (NS_FAILED(supStr->GetData(name)))
      continue;

    nsCAutoString fullName(aPrefix);
    fullName.Append(name);
    NS_ENSURE_TRUE(array->AppendElement(fullName), NS_ERROR_OUT_OF_MEMORY);
  }
    
  return NS_NewAdoptingUTF8StringEnumerator(aResult, array);
}

// we should change the interface so that we can just pass back a enumerator!
NS_IMETHODIMP
nsCharsetConverterManager::GetDecoderList(nsIUTF8StringEnumerator ** aResult)
{
  return GetList(NS_LITERAL_CSTRING(NS_UNICODEDECODER_NAME),
                 EmptyCString(), aResult);
}

NS_IMETHODIMP
nsCharsetConverterManager::GetEncoderList(nsIUTF8StringEnumerator ** aResult)
{
  return GetList(NS_LITERAL_CSTRING(NS_UNICODEENCODER_NAME),
                 EmptyCString(), aResult);
}

NS_IMETHODIMP
nsCharsetConverterManager::GetCharsetDetectorList(nsIUTF8StringEnumerator** aResult)
{
  return GetList(NS_LITERAL_CSTRING("charset-detectors"),
                 NS_LITERAL_CSTRING("chardet."), aResult);
}

// XXX Improve the implementation of this method. Right now, it is build on 
// top of the nsCharsetAlias service. We can make the nsCharsetAlias
// better, with its own hash table (not the StringBundle anymore) and
// a nicer file format.
NS_IMETHODIMP
nsCharsetConverterManager::GetCharsetAlias(const char * aCharset, 
                                           nsACString& aResult)
{
  NS_ENSURE_ARG_POINTER(aCharset);

  // We try to obtain the preferred name for this charset from the charset 
  // aliases.
  nsresult rv;

  rv = nsCharsetAlias::GetPreferred(nsDependentCString(aCharset), aResult);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}


NS_IMETHODIMP
nsCharsetConverterManager::GetCharsetTitle(const char * aCharset, 
                                           nsAString& aResult)
{
  NS_ENSURE_ARG_POINTER(aCharset);

  if (mTitleBundle == NULL) {
    nsresult rv = LoadExtensibleBundle(NS_TITLE_BUNDLE_CATEGORY, &mTitleBundle);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return GetBundleValue(mTitleBundle, aCharset, NS_LITERAL_STRING(".title"), aResult);
}

NS_IMETHODIMP
nsCharsetConverterManager::GetCharsetData(const char * aCharset, 
                                          const PRUnichar * aProp,
                                          nsAString& aResult)
{
  if (aCharset == NULL)
    return NS_ERROR_NULL_POINTER;
  // aProp can be NULL

  if (mDataBundle == NULL) {
    nsresult rv = LoadExtensibleBundle(NS_DATA_BUNDLE_CATEGORY, &mDataBundle);
    if (NS_FAILED(rv))
      return rv;
  }

  return GetBundleValue(mDataBundle, aCharset, nsDependentString(aProp), aResult);
}

NS_IMETHODIMP
nsCharsetConverterManager::GetCharsetLangGroup(const char * aCharset, 
                                               nsIAtom** aResult)
{
  // resolve the charset first
  nsCAutoString charset;

  nsresult rv = GetCharsetAlias(aCharset, charset);
  NS_ENSURE_SUCCESS(rv, rv);

  // fully qualify to possibly avoid vtable call
  return nsCharsetConverterManager::GetCharsetLangGroupRaw(charset.get(),
                                                           aResult);
}

NS_IMETHODIMP
nsCharsetConverterManager::GetCharsetLangGroupRaw(const char * aCharset, 
                                                  nsIAtom** aResult)
{

  *aResult = nsnull;
  if (aCharset == NULL)
    return NS_ERROR_NULL_POINTER;

  nsresult rv = NS_OK;

  if (mDataBundle == NULL) {
    rv = LoadExtensibleBundle(NS_DATA_BUNDLE_CATEGORY, &mDataBundle);
    if (NS_FAILED(rv))
      return rv;
  }

  nsAutoString langGroup;
  rv = GetBundleValue(mDataBundle, aCharset, NS_LITERAL_STRING(".LangGroup"), langGroup);

  if (NS_SUCCEEDED(rv)) {
    ToLowerCase(langGroup); // use lowercase for all language atoms
    *aResult = NS_NewAtom(langGroup);
  }

  return rv;
}
