// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/moru_screen.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <winrt/Windows.Foundation.h>
#include "common/scope_exit.h"
#include "core/hle/service/mii/mii_database_manager.h"
#include "core/hle/service/mii/mii_util.h"
#include "core/hle/service/mii/types/char_info.h"
#include "eden_uwp/applet_boot.h"
#include "eden_uwp/diagnostic.h"
#include "eden_uwp/firmware.h"
#include "eden_uwp/moru_parts.h"
#include "eden_uwp/ui/strings.h"
#include "eden_uwp/ui/text_entry.h"
#include "eden_uwp/ui/widgets.h"

namespace EdenXbox::Ui {
namespace {
using D2D1::RectF;
using Service::Mii::StoreData;
using winrt::Windows::UI::Core::CoreWindow;
constexpr std::array<Text, Moru::FieldCount> Labels = {
    Text::MoruFace,         Text::MoruSkin,     Text::MoruHair,     Text::MoruHairColor,
    Text::MoruBrows,        Text::MoruEyes,     Text::MoruEyeColor, Text::MoruNose,
    Text::MoruMouth,        Text::MoruBeard,    Text::MoruMustache, Text::MoruGlasses,
    Text::MoruGlassesColor, Text::MoruFavorite, Text::MoruGender,   Text::MoruHeight,
    Text::MoruBuild};
constexpr int NicknameRow = Moru::FieldCount;
constexpr int RandomRow = NicknameRow + 1;
constexpr int SaveRow = RandomRow + 1;
constexpr int DiscardRow = SaveRow + 1;
constexpr int DeleteRow = DiscardRow + 1;
constexpr int VisibleRows = 8;

std::wstring Nickname(const StoreData& data) {
    const auto name = data.GetNickname();
    const auto end = std::find(name.data.begin(), name.data.end(), char16_t{});
    return std::wstring(name.data.begin(), end);
}

class MoruScreen {
public:
    MoruScreen(Renderer& renderer, const CoreWindow& window, Input& input,
               std::optional<StoreData>* selected, std::string* launch_target)
        : renderer_(renderer), window_(window), input_(input), selected_(selected),
          launch_target_(launch_target),
          system_creator_(launch_target && !selected && GetFirmwareStatus().mii_edit_available) {
        if (selected_)
            selected_->reset();
        sample_ = NewDraft();
        Load();
    }
    bool Run() {
        const InputTransition transition(input_);
        const auto token = window_.Closed([this](const auto&, const auto&) { closed_ = true; });
        SCOPE_EXIT {
            window_.Closed(token);
        };
        Diagnostic("MORU open");
        while (!closed_ && !done_) {
            window_.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            input_.Update();
            Update();
            renderer_.BeginFrame();
            Draw();
            renderer_.EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        Diagnostic("MORU close");
        return closed_;
    }

private:
    int CreateRow() const {
        return system_creator_ ? 1 : 0;
    }
    int ListCount() const {
        return static_cast<int>(entries_.size()) + CreateRow() + 1;
    }
    enum class Page { List, Editor, Discard, Delete };
    StoreData NewDraft() const {
        StoreData data;
        data.BuildDefault(0); // Own default; creates an RFC4122 ID via the service.
        for (int i = 0; i < Moru::FieldCount; ++i) {
            const auto field = static_cast<Moru::Field>(i);
            if (field != Moru::Field::Height && field != Moru::Field::Build)
                Moru::Choose(data, field, 0);
        }
        const std::wstring name = Tr(Text::MoruDefaultName);
        Moru::Rename(data, std::u16string(name.begin(), name.end()));
        return data;
    }
    bool Load() {
        // Never offer writes after a failed load: Initialize may clean its in-memory
        // database on corruption. In particular, do not save that empty replacement.
        database_ = std::make_unique<Service::Mii::DatabaseManager>();
        metadata_ = {};
        entries_.clear();
        bool broken = false;
        ready_ = database_->MountSaveData().IsSuccess() &&
                 database_->Initialize(metadata_, broken).IsSuccess() && !broken;
        if (!ready_)
            return false;
        for (u32 i = 0; i < database_->GetCount(metadata_); ++i) {
            StoreData data;
            database_->Get(data, i, metadata_);
            entries_.push_back(data);
        }
        list_row_ = std::clamp(list_row_, 0, ListCount() - 1);
        if (page_ == Page::List)
            row_ = std::clamp(row_, 0, ListCount() - 1);
        return true;
    }
    void Edit(bool create) {
        if (create && database_->IsFullDatabase()) {
            full_ = true;
            return;
        }
        creating_ = create;
        if (create) {
            draft_ = NewDraft();
        } else {
            draft_ = entries_.at(static_cast<std::size_t>(row_ - CreateRow() - 1));
        }
        list_row_ = row_;
        row_ = 0;
        page_ = Page::Editor;
        failed_ = full_ = false;
    }
    bool Save(bool remove) {
        Service::Mii::CharInfo info;
        info.SetFromStoreData(draft_);
        if (!remove && (draft_.IsValid() != Service::Mii::ValidationResult::NoErrors ||
                        info.Verify() != Service::Mii::ValidationResult::NoErrors))
            return false;
        // Refresh before mutation, so a stale screen cannot overwrite other saved entries.
        if (!Load())
            return false;
        s32 existing{};
        const bool found = database_->FindIndex(existing, draft_.GetCreateId(), false).IsSuccess();
        if ((!creating_ && !found) || (creating_ && found))
            return false;
        if (creating_ && database_->IsFullDatabase()) {
            full_ = true;
            return false;
        }
        const auto result = remove ? database_->Delete(metadata_, draft_.GetCreateId())
                                   : database_->AddOrReplace(metadata_, draft_);
        if (result.IsError() || database_->SaveDatabase().IsError()) {
            Load();
            return false;
        }
        if (!Load())
            return false;
        page_ = Page::List;
        row_ = std::min(list_row_, ListCount() - 1);
        return true;
    }
    void Update() {
        if (entry_) {
            const auto result = entry_->Update(input_);
            if (result == TextEntry::Result::Editing)
                return;
            if (result == TextEntry::Result::Confirmed) {
                const auto& name = entry_->Text();
                failed_ = !Moru::Rename(draft_, std::u16string(name.begin(), name.end()));
            }
            entry_.reset();
            input_.ConsumeUntilRelease();
            return;
        }
        if (input_.Pressed(Button::B)) {
            if (page_ == Page::List)
                done_ = true;
            else if (page_ == Page::Editor) {
                page_ = Page::Discard;
                row_ = 0;
            } else {
                page_ = Page::Editor;
                row_ = DiscardRow;
            }
            input_.ConsumeUntilRelease();
            return;
        }
        if (!ready_) {
            if (input_.Pressed(Button::A))
                Load();
            return;
        }
        const int count = page_ == Page::List     ? ListCount()
                          : page_ == Page::Editor ? (creating_ ? DeleteRow : DeleteRow + 1)
                                                  : 2;
        if (input_.Pressed(Button::Up))
            row_ = std::max(0, row_ - 1);
        if (input_.Pressed(Button::Down))
            row_ = std::min(count - 1, row_ + 1);
        if (page_ == Page::Editor && row_ < Moru::FieldCount) {
            const auto field = static_cast<Moru::Field>(row_);
            int choice = Moru::Choice(draft_, field);
            if (input_.Pressed(Button::Left))
                Moru::Choose(draft_, field, choice - 1);
            if (input_.Pressed(Button::Right) || input_.Pressed(Button::A))
                Moru::Choose(draft_, field, choice + 1);
            return;
        }
        if (page_ == Page::List && row_ > CreateRow() && input_.Pressed(Button::X)) {
            Edit(false);
            input_.ConsumeUntilRelease();
            return;
        }
        if (!input_.Pressed(Button::A))
            return;
        switch (page_) {
        case Page::List:
            if (system_creator_ && row_ == 0) {
                *launch_target_ = std::string(SwitchMiiEditBootTarget);
                done_ = true;
            } else if (row_ == CreateRow())
                Edit(true);
            else if (selected_) {
                *selected_ = entries_[static_cast<std::size_t>(row_ - CreateRow() - 1)];
                done_ = true;
            } else
                Edit(false);
            break;
        case Page::Editor:
            if (row_ == NicknameRow)
                entry_ = std::make_unique<TextEntry>(Tr(Text::MoruNickname), Nickname(draft_));
            else if (row_ == RandomRow) {
                // Choose only from our original catalogue; keep identity and nickname.
                for (int i = 0; i < Moru::FieldCount; ++i) {
                    const auto field = static_cast<Moru::Field>(i);
                    Moru::Choose(draft_, field,
                                 Service::Mii::MiiUtil::GetRandomValue<int>(
                                     0, static_cast<int>(Moru::Options(field).size()) - 1));
                }
            } else if (row_ == SaveRow)
                failed_ = !Save(false);
            else if (row_ == DiscardRow) {
                page_ = Page::Discard;
                row_ = 0;
            } else if (row_ == DeleteRow) {
                page_ = Page::Delete;
                row_ = 0;
            }
            break;
        case Page::Discard:
            if (row_ == 1) {
                page_ = Page::List;
                row_ = list_row_;
                failed_ = full_ = false;
            } else {
                page_ = Page::Editor;
                row_ = DiscardRow;
            }
            break;
        case Page::Delete:
            if (row_ == 1)
                failed_ = !Save(true);
            else {
                page_ = Page::Editor;
                row_ = DeleteRow;
            }
            break;
        }
        input_.ConsumeUntilRelease();
    }
    void DrawPreview(const StoreData& data) {
        constexpr float scale = 1.9f;
        for (const auto& part : Moru::MakeScene(data)) {
            renderer_.FillRounded(RectF(1160 + part.x * scale, 245 + part.y * scale,
                                        1160 + (part.x + part.width) * scale,
                                        245 + (part.y + part.height) * scale),
                                  part.radius * scale, Theme::Rgb(part.color));
        }
        renderer_.DrawString(Nickname(data), Font::Section, RectF(1100, 780, 1750, 850),
                             Theme::kText, HAlign::Center);
    }
    void DrawRow(int visible, bool focused, const std::wstring& label,
                 const std::wstring& value = L"", bool destructive = false) {
        const float y = 250 + visible * 76.0f;
        const auto rect = RectF(96, y, 1050, y + 64);
        renderer_.DrawString(label, Font::Nav, RectF(120, y, 790, y + 64),
                             destructive ? Theme::kDangerText : Theme::kText, HAlign::Left,
                             VAlign::Middle);
        renderer_.DrawString(value, Font::MetaMono, RectF(780, y, 1020, y + 64),
                             Theme::kTextSecondary, HAlign::Right, VAlign::Middle);
        if (focused)
            DrawRing(renderer_, rect, 32, 1);
    }
    void Draw() {
        const auto title = page_ == Page::List      ? Text::MoruTitle
                           : page_ == Page::Delete  ? Text::MoruDeleteQuestion
                           : page_ == Page::Discard ? Text::MoruDiscard
                                                    : Text::MoruEdit;
        renderer_.DrawString(Tr(title), Font::Heading, RectF(96, 115, 1800, 210), Theme::kText);
        if (!ready_) {
            renderer_.DrawString(Tr(Text::MoruLoadFailed), Font::Body, RectF(96, 300, 1700, 500),
                                 Theme::kTextSecondary);
        } else if (page_ == Page::List) {
            const int count = ListCount();
            const int first = std::clamp(row_ - 3, 0, std::max(0, count - VisibleRows));
            for (int i = first; i < std::min(count, first + VisibleRows); ++i)
                DrawRow(i - first, i == row_,
                        system_creator_ && i == 0 ? Tr(Text::MoruSystemCreator)
                        : i == CreateRow()        ? Tr(Text::MoruCreate)
                                                  : Nickname(entries_[i - CreateRow() - 1]));
            if (system_creator_ && row_ == 0) {
                renderer_.DrawString(Tr(Text::MoruSystemCreatorHint), Font::Body,
                                     RectF(1100, 300, 1760, 600), Theme::kTextSecondary);
            } else if (row_ > CreateRow())
                DrawPreview(entries_[row_ - CreateRow() - 1]);
            else if (entries_.empty()) {
                // The empty state shows the next step and an original face, without persisting it.
                DrawPreview(sample_);
                renderer_.DrawString(Tr(Text::MoruEmpty), Font::Body, RectF(1100, 850, 1760, 960),
                                     Theme::kTextSecondary, HAlign::Center);
            }
        } else {
            DrawPreview(draft_);
            if (page_ == Page::Editor) {
                const int count = creating_ ? DeleteRow : DeleteRow + 1;
                const int first = std::clamp(row_ - 3, 0, std::max(0, count - VisibleRows));
                for (int i = first; i < std::min(count, first + VisibleRows); ++i) {
                    if (i < Moru::FieldCount) {
                        const auto field = static_cast<Moru::Field>(i);
                        const int choice = Moru::Choice(draft_, field);
                        std::wstring value = choice < 0
                                                 ? Tr(Text::MoruOriginal)
                                                 : std::to_wstring(choice + 1) + L" / " +
                                                       std::to_wstring(Moru::Options(field).size());
                        if (field == Moru::Field::Height || field == Moru::Field::Build)
                            value = std::to_wstring(Moru::Value(draft_, field));
                        if (field == Moru::Field::Gender)
                            value = Tr(draft_.GetGender() == Service::Mii::Gender::Female
                                           ? Text::MoruFemale
                                           : Text::MoruMale);
                        DrawRow(i - first, i == row_, Tr(Labels[i]), value);
                        if (i == row_ &&
                            (field == Moru::Field::Height || field == Moru::Field::Build ||
                             field == Moru::Field::Gender)) {
                            const float y = 250 + (i - first) * 76.0f + 60;
                            renderer_.FillRounded(RectF(125, y, 760, y + 4), 2,
                                                  Theme::kSurfaceStrong);
                            const float fraction = static_cast<float>(Moru::Value(draft_, field)) /
                                                   (field == Moru::Field::Gender ? 1.0f : 127.0f);
                            renderer_.FillDisc(D2D1::Point2F(125 + fraction * 635, y + 2), 7,
                                               Theme::kText);
                        }
                    } else {
                        const Text label = i == NicknameRow  ? Text::MoruNickname
                                           : i == RandomRow  ? Text::MoruRandom
                                           : i == SaveRow    ? Text::MoruSave
                                           : i == DiscardRow ? Text::MoruDiscard
                                                             : Text::MoruDelete;
                        DrawRow(i - first, i == row_, Tr(label), L"", i == DeleteRow);
                    }
                }
            } else {
                DrawRow(0, row_ == 0, Tr(Text::HintCancel));
                DrawRow(1, row_ == 1,
                        Tr(page_ == Page::Delete ? Text::MoruDelete : Text::MoruDiscard), L"",
                        page_ == Page::Delete);
            }
        }
        std::vector<Hint> hints = {{Theme::kButtonA, L"A", Tr(Text::HintSelect)},
                                   {Theme::kButtonB, L"B", Tr(Text::HintBack)}};
        if (page_ == Page::List && row_ > CreateRow())
            hints.push_back({Theme::kButtonX, L"X", Tr(Text::MoruEdit)});
        if (page_ == Page::Editor)
            hints.push_back({Theme::kTextSecondary, L"↔", Tr(Text::MoruAdjust)});
        DrawHints(renderer_, hints);
        if (full_ || failed_)
            DrawToastCapsule(renderer_, Tr(full_ ? Text::MoruFull : Text::MoruFailed), 1);
        if (entry_)
            entry_->Draw(renderer_, Clock::now());
    }
    Renderer& renderer_;
    CoreWindow window_;
    Input& input_;
    std::optional<StoreData>* selected_;
    std::string* launch_target_;
    bool system_creator_ = false;
    std::unique_ptr<Service::Mii::DatabaseManager> database_;
    Service::Mii::DatabaseSessionMetadata metadata_{};
    std::vector<StoreData> entries_;
    StoreData draft_{};
    StoreData sample_{};
    std::unique_ptr<TextEntry> entry_;
    Page page_ = Page::List;
    int row_ = 0, list_row_ = 0;
    bool ready_ = false, creating_ = false, failed_ = false, full_ = false;
    bool closed_ = false, done_ = false;
};
} // namespace
bool RunMoruScreen(Renderer& renderer, const CoreWindow& window, Input& input,
                   std::optional<StoreData>* selected, std::string* launch_target) {
    MoruScreen screen(renderer, window, input, selected, launch_target);
    return screen.Run();
}
} // namespace EdenXbox::Ui
