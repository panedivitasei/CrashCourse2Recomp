#pragma once

#include "lg_economy_file.h"
#include <vector>

namespace cc2::kernel::xam {

inline uint32_t* LgAccount(LgEconomyFile& economy, uint32_t id) {
  switch (id) {
    case 101: return &economy.stars;
    case 102: return &economy.total_stars;
    case 103: return &economy.coins;
    case 104: return &economy.rewinds;
  }
  for (const auto& item : kLgConsumables) {
    if (item.account_id == id) return &economy.consumables[item.currency];
  }
  return nullptr;
}

inline bool LgCredit(uint32_t& balance, uint64_t amount) {
  if (amount > std::numeric_limits<uint32_t>::max() - balance) return false;
  balance += static_cast<uint32_t>(amount);
  return true;
}

// Price lookup returns the catalog price in the account selected by the native purchase task.
template <typename PriceLookup>
bool LgPurchase(LgEconomyFile& economy, const std::string& xml, PriceLookup price,
                std::string& reason) {
  const auto payment = xml.find("<tfeAccountId>");
  const auto payment_end = xml.find("</tfeAccountId>", payment);
  uint32_t account = 0;
  if (xml.find("<purchaseOrder ") == std::string::npos ||
      xml.find("</purchaseOrder>") == std::string::npos ||
      xml.find("<orderCommand>CHARGE</orderCommand>") == std::string::npos ||
      payment == std::string::npos || payment_end == std::string::npos ||
      !LgEconomyFile::ReadUInt(std::string_view(xml).substr(payment + 14,
                              payment_end - payment - 14), account) ||
      (account != 101 && account != 103)) {
    reason = "invalid purchase or payment account";
    return false;
  }
  LgEconomyFile next = economy;
  uint64_t cost = 0;
  size_t lines = 0;
  std::set<uint32_t> requested;
  for (size_t at = xml.find("<itemOfferRequest "); at != std::string::npos;
       at = xml.find("<itemOfferRequest ", at + 1)) {
    uint32_t id, amount;
    if (!LgEconomyFile::ReadAttr(xml, at, "id", id) || !id ||
        !price(id, account, amount)) {
      reason = "unknown item offer";
      return false;
    }
    ++lines;
    if (requested.insert(id).second && next.owned.insert(id).second) cost += amount;
  }
  for (size_t at = xml.find("<forexOfferRequest "); at != std::string::npos;
       at = xml.find("<forexOfferRequest ", at + 1)) {
    uint32_t id, amount;
    if (!LgEconomyFile::ReadAttr(xml, at, "id", id) ||
        !LgEconomyFile::ReadAttr(xml, at, "amountFromCurrency", amount)) {
      reason = "invalid forex amount";
      return false;
    }
    const LgConsumableDef* offer = nullptr;
    for (const auto& item : kLgConsumables) {
      if (item.offer_id == id) { offer = &item; break; }
    }
    if (!offer || account != 101 || !amount || !offer->price_stars ||
        amount % offer->price_stars) {
      reason = "unknown forex offer or non-integral quantity";
      return false;
    }
    ++lines;
    cost += amount;
    const uint64_t quantity = uint64_t(amount / offer->price_stars) * offer->qty;
    if (!LgCredit(*LgAccount(next, offer->account_id), quantity)) {
      reason = "inventory overflow";
      return false;
    }
  }
  if (!lines || cost > *LgAccount(next, account)) {
    reason = lines ? "insufficient balance" : "empty purchase";
    return false;
  }
  *LgAccount(next, account) -= static_cast<uint32_t>(cost);
  economy = std::move(next);
  return true;
}

// Apply the complete native transfer batch or leave all balances unchanged.
inline bool LgTransfers(LgEconomyFile& economy, const std::string& xml,
                        std::string& reason) {
  if (xml.find("<currencyTransfers") == std::string::npos ||
      xml.find("</currencyTransfers>") == std::string::npos) {
    reason = "invalid transfer document";
    return false;
  }
  LgEconomyFile next = economy;
  size_t count = 0;
  for (size_t at = xml.find("<currencyTransfer>"); at != std::string::npos;
       at = xml.find("<currencyTransfer>", at + 1)) {
    const auto end = xml.find("</currencyTransfer>", at);
    if (end == std::string::npos) { reason = "unterminated transfer"; return false; }
    const auto block = xml.substr(at, end - at);
    uint32_t debit, credit, amount;
    if (!LgEconomyFile::ReadAttr(block, block.find("<debitAccount"), "id", debit) ||
        !LgEconomyFile::ReadAttr(block, block.find("<creditAccount"), "id", credit) ||
        !LgEconomyFile::ReadAttr(block, block.find("<money"), "amount", amount) ||
        !debit || !credit) {
      reason = "invalid transfer accounts or amount";
      return false;
    }
    auto* from = LgAccount(next, debit);
    auto* to = LgAccount(next, credit);
    if (!from && !to) { reason = "transfer has no player account"; return false; }
    if (from && *from < amount) { reason = "insufficient transfer balance"; return false; }
    if (from) *from -= amount;
    if (to && !LgCredit(*to, amount)) { reason = "transfer overflow"; return false; }
    ++count;
  }
  if (!count) { reason = "empty transfer batch"; return false; }
  economy = std::move(next);
  return true;
}

}  // namespace cc2::kernel::xam
