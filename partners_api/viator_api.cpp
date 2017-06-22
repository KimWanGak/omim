#include "partners_api/viator_api.hpp"

#include "platform/http_client.hpp"
#include "platform/preferred_languages.hpp"

#include "coding/multilang_utf8_string.hpp"

#include "base/logging.hpp"
#include "base/thread.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "3party/jansson/myjansson.hpp"

#include "private.h"

namespace
{
using namespace platform;
using namespace viator;

std::string const kApiUrl = "https://viatorapi.viator.com";
std::string const kWebUrl = "https://www.partner.viator.com";

int8_t GetLang(std::string const & lang)
{
  return StringUtf8Multilang::GetLangIndex(lang);
}

using IdsMap = std::unordered_map<int8_t, std::string>;

IdsMap kApiKeys =
{
  {GetLang("en"), VIATOR_API_KEY_EN},
  {GetLang("de"), VIATOR_API_KEY_DE},
  {GetLang("fr"), VIATOR_API_KEY_FR},
  {GetLang("es"), VIATOR_API_KEY_ES},
  {GetLang("pt"), VIATOR_API_KEY_PT},
  {GetLang("it"), VIATOR_API_KEY_IT},
  {GetLang("nl"), VIATOR_API_KEY_NL},
  {GetLang("sv"), VIATOR_API_KEY_SV},
  {GetLang("ja"), VIATOR_API_KEY_JA}
};

IdsMap kAccountIds =
{
  {GetLang("en"), VIATOR_ACCOUNT_ID_EN},
  {GetLang("de"), VIATOR_ACCOUNT_ID_DE},
  {GetLang("fr"), VIATOR_ACCOUNT_ID_FR},
  {GetLang("es"), VIATOR_ACCOUNT_ID_ES},
  {GetLang("pt"), VIATOR_ACCOUNT_ID_PT},
  {GetLang("it"), VIATOR_ACCOUNT_ID_IT},
  {GetLang("nl"), VIATOR_ACCOUNT_ID_NL},
  {GetLang("sv"), VIATOR_ACCOUNT_ID_SV},
  {GetLang("ja"), VIATOR_ACCOUNT_ID_JA}
};

std::string GetId(IdsMap const & from)
{
  int8_t const lang = GetLang(languages::GetCurrentNorm());

  auto const it = from.find(lang);

  if (it != from.cend())
    return it->second;

  LOG(LINFO, ("Viator key for language", lang, "is not found, English key will be used."));
  return from.at(StringUtf8Multilang::kEnglishCode);
}

std::string GetApiKey()
{
  return GetId(kApiKeys);
}

std::string GetAccountId()
{
  return GetId(kAccountIds);
}

bool RunSimpleHttpRequest(std::string const & url, std::string const & bodyData,
                          std::string & result)
{
  HttpClient request(url);
  request.SetHttpMethod("POST");

  request.SetBodyData(bodyData, "application/json");
  if (request.RunHttpRequest() && !request.WasRedirected() && request.ErrorCode() == 200)
  {
    result = request.ServerResponse();
    return true;
  }

  return false;
}

std::string MakeSearchProductsRequest(int destId, std::string const & currency, int count)
{
  std::ostringstream os;
  // REVIEW_AVG_RATING_D - average traveler rating (high->low).
  os << R"({"topX":"1-)" << count << R"(","destId":)" << destId << R"(,"currencyCode":")"
     << currency << R"(","sortOrder":"REVIEW_AVG_RATING_D"})";

  return os.str();
}

std::string MakeUrl(std::string const & apiMethod)
{
  std::ostringstream os;
  os << kApiUrl << apiMethod << "?apiKey=" << GetApiKey();

  return os.str();
}

bool CheckJsonArray(json_t const * data)
{
  if (data == nullptr)
    return false;

  if (!json_is_array(data))
    return false;

  if (json_array_size(data) <= 0)
    return false;

  return true;
}

bool CheckAnswer(my::Json const & root)
{
  bool success;
  FromJSONObjectOptionalField(root.get(), "success", success, false);

  if (!success)
  {
    std::string errorMessage = "Unknown error.";
    auto const errorMessageArray = json_object_get(root.get(), "errorMessageText");

    if (CheckJsonArray(errorMessageArray))
      FromJSON(json_array_get(errorMessageArray, 0), errorMessage);

    LOG(LWARNING, ("Viator retrieved unsuccessfull status, error message:", errorMessage));
  }

  return success;
}

void MakeProducts(std::string const & src, std::vector<Product> & products)
{
  products.clear();

  my::Json root(src.c_str());
  auto const data = json_object_get(root.get(), "data");
  if (!CheckAnswer(root) || !CheckJsonArray(data))
    return;

  auto const dataSize = json_array_size(data);
  for (size_t i = 0; i < dataSize; ++i)
  {
    Product product;
    auto const item = json_array_get(data, i);
    FromJSONObject(item, "shortTitle", product.m_title);
    FromJSONObject(item, "rating", product.m_rating);
    FromJSONObject(item, "reviewCount", product.m_reviewCount);
    FromJSONObject(item, "duration", product.m_duration);
    FromJSONObject(item, "price", product.m_price);
    FromJSONObject(item, "priceFormatted", product.m_priceFormatted);
    FromJSONObject(item, "currencyCode", product.m_currency);
    FromJSONObject(item, "thumbnailHiResURL", product.m_photoUrl);
    FromJSONObject(item, "webURL", product.m_pageUrl);
    products.push_back(move(product));
  }
}
}  // namespace

namespace viator
{
// static
bool RawApi::GetTopProducts(std::string const & destId, std::string const & currency, int count,
                            std::string & result)
{
  int dest = 0;
  CHECK(strings::to_int(destId, dest), ());

  return RunSimpleHttpRequest(MakeUrl("/service/search/products"),
                              MakeSearchProductsRequest(dest, currency, count), result);
}

// static
std::string Api::GetCityUrl(std::string const & destId, std::string const & name)
{
  std::ostringstream ost;
  // The final language and city name will be calculated automatically based on account id and
  // destination id.
  ost << kWebUrl << "/" << languages::GetCurrentNorm() << "/" << GetAccountId() << "/" << name
      << "/d" << destId << "-ttd?activities=all";
  return ost.str();
}

void Api::GetTop5Products(std::string const & destId, std::string const & currency,
                          GetTop5ProductsCallback const & fn) const
{
  std::string curr = currency.empty() ? "USD" : currency;

  threads::SimpleThread([destId, curr, fn]()
  {
    string result;
    if (!RawApi::GetTopProducts(destId, curr, 5, result))
      return fn(destId, {});

    std::vector<Product> products;
    try
    {
      MakeProducts(result, products);
    }
    catch (my::Json::Exception const & e)
    {
      LOG(LERROR, (e.Msg()));
      products.clear();
    }

    SortProducts(products);

    fn(destId, products);
  }).detach();
}

bool operator<(Product const & lhs, Product const & rhs)
{
  return lhs.m_rating < rhs.m_rating ||
      (lhs.m_rating == rhs.m_rating && lhs.m_reviewCount < rhs.m_reviewCount) ||
      (lhs.m_reviewCount == rhs.m_reviewCount && lhs.m_price < rhs.m_price);
}

// Sort by rating (from the best to the worst),
// then by reviews (from the largest to the smallest),
// then by price (from the biggest to the lowest)
void SortProducts(std::vector<Product> & products)
{
  std::multiset<Product> productsSet;
  for (auto const & p : products)
    productsSet.insert(p);

  std::copy(productsSet.crbegin(), productsSet.crend(), products.begin());
}
}  // namespace viator
