/**********************************************************************

  Audacity: A Digital Audio Editor

  BatchProcessDialog.cpp

  Dominic Mazzoni
  James Crook

*******************************************************************//*!

\class BatchProcessDialog
\brief Shows progress in executing commands in BatchCommands.

*//*******************************************************************/

#include "Audacity.h"
#include "BatchProcessDialog.h"

#include <wx/defs.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/intl.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/listctrl.h>
#include <wx/radiobut.h>
#include <wx/button.h>
#include <wx/imaglist.h>
#include <wx/settings.h>

#include "AudacityException.h"
#include "ShuttleGui.h"
#include "Prefs.h"
#include "Project.h"
#include "Internat.h"
#include "commands/CommandManager.h"
#include "commands/CommandContext.h"
#include "effects/Effect.h"
#include "../images/Arrow.xpm"
#include "../images/Empty9x16.xpm"
#include "BatchCommands.h"
#include "Track.h"
#include "UndoManager.h"

#include "Theme.h"
#include "AllThemeResources.h"

#include "FileDialog.h"
#include "FileNames.h"
#include "import/Import.h"
#include "widgets/ErrorDialog.h"
#include "widgets/HelpSystem.h"

#define ChainsListID       7001
#define CommandsListID     7002
#define ApplyToProjectID   7003
#define ApplyToFilesID     7004

BEGIN_EVENT_TABLE(BatchProcessDialog, wxDialogWrapper)
   EVT_BUTTON(ApplyToProjectID, BatchProcessDialog::OnApplyToProject)
   EVT_BUTTON(ApplyToFilesID, BatchProcessDialog::OnApplyToFiles)
   EVT_BUTTON(wxID_CANCEL, BatchProcessDialog::OnCancel)
   EVT_BUTTON(wxID_HELP, BatchProcessDialog::OnHelp)
END_EVENT_TABLE()

BatchProcessDialog::BatchProcessDialog(wxWindow * parent, bool bInherited):
   wxDialogWrapper(parent, wxID_ANY, _("Apply Chain"),
            wxDefaultPosition, wxDefaultSize,
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
   //AudacityProject * p = GetActiveProject();
   mAbort = false;
   if( bInherited )
      return;
   SetLabel(_("Apply Chain"));         // Provide visual label
   SetName(_("Apply Chain"));          // Provide audible label
   Populate();

}

BatchProcessDialog::~BatchProcessDialog()
{
}

void BatchProcessDialog::Populate()
{
   //------------------------- Main section --------------------
   ShuttleGui S(this, eIsCreating);
   PopulateOrExchange(S);
   // ----------------------- End of main section --------------
   // Get and validate the currently active chain
   mActiveChain = gPrefs->Read(wxT("/Batch/ActiveChain"), wxT(""));
   // Go populate the chains list.
   PopulateChains();

   Layout();
   Fit();
   SetSizeHints(GetSize());
   Center();

   // Set the column size for the chains list.
   wxSize sz = mChains->GetClientSize();
   mChains->SetColumnWidth(0, sz.x);
}

/// Defines the dialog and does data exchange with it.
void BatchProcessDialog::PopulateOrExchange(ShuttleGui &S)
{
   S.StartVerticalLay(true);
   {
      /*i18n-hint: A chain is a sequence of commands that can be applied
       * to one or more audio files.*/
      S.StartStatic(_("&Select Chain"), true);
      {
         S.SetStyle(wxSUNKEN_BORDER | wxLC_REPORT | wxLC_HRULES | wxLC_VRULES |
                     wxLC_SINGLE_SEL);
         mChains = S.Id(ChainsListID).AddListControlReportMode();
         mChains->InsertColumn(0, _("Chain"), wxLIST_FORMAT_LEFT);
      }
      S.EndStatic();

      S.StartHorizontalLay(wxALIGN_RIGHT, false);
      {
         S.SetBorder(10);
         S.AddPrompt( _("Apply Chain to:") );
         S.Id(ApplyToProjectID).AddButton(_("&Project"));
         S.Id(ApplyToFilesID).AddButton(_("&Files..."));
         S.AddSpace( 40 );
         S.AddStandardButtons( eCancelButton | eHelpButton);
      }
      S.EndHorizontalLay();
   }
   S.EndVerticalLay();
}

/// This clears and updates the contents of mChains, the list of chains.
void BatchProcessDialog::PopulateChains()
{
   wxArrayString names = mBatchCommands.GetNames();
   int i;

   mChains->DeleteAllItems();
   for (i = 0; i < (int)names.GetCount(); i++) {
      mChains->InsertItem(i, names[i]);
   }

   int item = mChains->FindItem(-1, mActiveChain);
   if (item == -1) {
      item = 0;
      mActiveChain = mChains->GetItemText(0);
   }

   // Select the name in the list...this will fire an event.
   mChains->SetItemState(item, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
}




void BatchProcessDialog::OnHelp(wxCommandEvent & WXUNUSED(event))
{
   wxString page = GetHelpPageName();
   HelpSystem::ShowHelp(this, page, true);
}

void BatchProcessDialog::OnApplyToProject(wxCommandEvent & WXUNUSED(event))
{
   long item = mChains->GetNextItem(-1,
                                    wxLIST_NEXT_ALL,
                                    wxLIST_STATE_SELECTED);

   if (item == -1) {
      AudacityMessageBox(_("No chain selected"));
      return;
   }
   ApplyChainToProject( item );
}

void BatchProcessDialog::ApplyChainToProject( int iChain, bool bHasGui )
{
   wxString name = mChains->GetItemText(iChain);
   if( name.IsEmpty() )
      return;

   wxDialogWrapper activityWin( this, wxID_ANY, GetTitle());
   activityWin.SetName(activityWin.GetTitle());
   ShuttleGui S(&activityWin, eIsCreating);

   S.StartHorizontalLay(wxCENTER, false);
   {
      S.StartStatic( {}, false);   // deliberately not translated (!)
      {
         S.SetBorder(20);
         S.AddFixedText(wxString::Format(_("Applying '%s' to current project"),
                                         name));
      }
      S.EndStatic();
   }
   S.EndHorizontalLay();

   activityWin.Layout();
   activityWin.Fit();
   activityWin.CenterOnScreen();
   // Avoid overlap with progress.
   int x,y;
   activityWin.GetPosition( &x, &y );
   activityWin.Move(wxMax(0,x-300), 0);
   activityWin.Show();

   // Without this the newly created dialog may not show completely.
   wxYield();

   // The Hide() on the next line seems to tickle a bug in wx3,
   // giving rise to our Bug #1221.  The problem is that on Linux 
   // the 'Hide' converts us from a Modal into a regular dialog,
   // as far as closing is concerned.  On Linux we can't close with
   // EndModal() anymore after this.
   if( bHasGui )
      Hide();

   gPrefs->Write(wxT("/Batch/ActiveChain"), name);
   gPrefs->Flush();

   mBatchCommands.ReadChain(name);

   // The disabler must get deleted before the EndModal() call.  Otherwise,
   // the menus on OSX will remain disabled.
   bool success;
   {
      wxWindowDisabler wd(&activityWin);
      success = GuardedCall< bool >(
         [this]{ return mBatchCommands.ApplyChain(); } );
   }

   if( !bHasGui )
      return;


   if (!success) {
      Show();
      return;
   }
   Hide();
}

void BatchProcessDialog::OnApplyToFiles(wxCommandEvent & WXUNUSED(event))
{
   long item = mChains->GetNextItem(-1,
                                    wxLIST_NEXT_ALL,
                                    wxLIST_STATE_SELECTED);
   if (item == -1) {
      AudacityMessageBox(_("No chain selected"));
      return;
   }

   wxString name = mChains->GetItemText(item);
   gPrefs->Write(wxT("/Batch/ActiveChain"), name);
   gPrefs->Flush();

   AudacityProject *project = GetActiveProject();
   if (!project->GetIsEmpty()) {
      AudacityMessageBox(_("Please save and close the current project first."));
      return;
   }

   wxString prompt =  _("Select file(s) for batch processing...");

   FormatList l;
   wxString filter;
   wxString all;

   Importer::Get().GetSupportedImportFormats(&l);
   for (const auto &format : l) {
      const Format *f = &format;

      wxString newfilter = f->formatName + wxT("|");
      for (size_t i = 0; i < f->formatExtensions.size(); i++) {
         if (!newfilter.Contains(wxT("*.") + f->formatExtensions[i] + wxT(";")))
            newfilter += wxT("*.") + f->formatExtensions[i] + wxT(";");
         if (!all.Contains(wxT("*.") + f->formatExtensions[i] + wxT(";")))
            all += wxT("*.") + f->formatExtensions[i] + wxT(";");
      }
      newfilter.RemoveLast(1);
      filter += newfilter;
      filter += wxT("|");
   }
   all.RemoveLast(1);
   filter.RemoveLast(1);

   wxString mask = _("All files|*|All supported files|") +
                   all + wxT("|") +
                   filter;

   wxString type = gPrefs->Read(wxT("/DefaultOpenType"),mask.BeforeFirst(wxT('|')));
   // Convert the type to the filter index
   int index = mask.First(type + wxT("|"));
   if (index == wxNOT_FOUND) {
      index = 0;
   }
   else {
      index = mask.Left(index).Freq(wxT('|')) / 2;
      if (index < 0) {
         index = 0;
      }
   }

   auto path = FileNames::FindDefaultPath(FileNames::Operation::Open);
   FileDialogWrapper dlog(this,
                   prompt,
                   path,
                   wxT(""),
                   mask,
                   wxFD_OPEN | wxFD_MULTIPLE | wxRESIZE_BORDER);

   dlog.SetFilterIndex(index);
   if (dlog.ShowModal() != wxID_OK) {
      return;
   }
   
   wxArrayString files;
   dlog.GetPaths(files);

   files.Sort();

   wxDialogWrapper activityWin(this, wxID_ANY, GetTitle());
   activityWin.SetName(activityWin.GetTitle());
   ShuttleGui S(&activityWin, eIsCreating);

   S.StartVerticalLay(false);
   {
      S.StartStatic(_("Applying..."), 1);
      {
         auto imageList = std::make_unique<wxImageList>(9, 16);
         imageList->Add(wxIcon(empty9x16_xpm));
         imageList->Add(wxIcon(arrow_xpm));

         S.SetStyle(wxSUNKEN_BORDER | wxLC_REPORT | wxLC_HRULES | wxLC_VRULES |
                    wxLC_SINGLE_SEL);
         mList = S.Id(CommandsListID).AddListControlReportMode();
         // AssignImageList takes ownership
         mList->AssignImageList(imageList.release(), wxIMAGE_LIST_SMALL);
         mList->InsertColumn(0, _("File"), wxLIST_FORMAT_LEFT);
      }
      S.EndStatic();

      S.StartHorizontalLay(wxCENTER, false);
      {
         S.Id(wxID_CANCEL).AddButton(_("&Cancel"));
      }
      S.EndHorizontalLay();
   }
   S.EndVerticalLay();

   int i;
   for (i = 0; i < (int)files.GetCount(); i++ ) {
      mList->InsertItem(i, files[i], i == 0);
   }

   // Set the column size for the files list.
   mList->SetColumnWidth(0, wxLIST_AUTOSIZE);

   int width = mList->GetColumnWidth(0);
   wxSize sz = mList->GetClientSize();
   if (width > sz.GetWidth() && width < 500) {
      sz.SetWidth(width);
      mList->SetInitialSize(sz);
   }

   activityWin.Layout();
   activityWin.Fit();
   activityWin.CenterOnScreen();
   // Avoid overlap with progress.
   int x,y;
   activityWin.GetPosition( &x, &y );
   activityWin.Move(wxMax(0,x-300), 0);
   activityWin.Show();

   // Without this the newly created dialog may not show completely.
   wxYield();
   Hide();

   mBatchCommands.ReadChain(name);
   for (i = 0; i < (int)files.GetCount(); i++) {
      wxWindowDisabler wd(&activityWin);
      if (i > 0) {
         //Clear the arrow in previous item.
         mList->SetItemImage(i - 1, 0, 0);
      }
      mList->SetItemImage(i, 1, 1);
      mList->EnsureVisible(i);

      auto success = GuardedCall< bool >( [&] {
         project->Import(files[i]);
         project->ZoomAfterImport(nullptr);
         project->OnSelectAll(*project);
         if (!mBatchCommands.ApplyChain())
            return false;

         if (!activityWin.IsShown() || mAbort)
            return false;

         return true;
      } );

      if (!success)
         break;
      
      UndoManager *um = project->GetUndoManager();
      um->ClearStates();
      project->OnSelectAll(*project);
      project->OnRemoveTracks(*project);
   }
   project->OnRemoveTracks(*project);
   Hide();
}

void BatchProcessDialog::OnCancel(wxCommandEvent & WXUNUSED(event))
{
   Hide();
}

/////////////////////////////////////////////////////////////////////
#include <wx/textdlg.h>
#include "BatchCommandDialog.h"

enum {
   AddButtonID = 10000,
   RemoveButtonID,
   ImportButtonID,
   ExportButtonID,
   DefaultsButtonID,
   InsertButtonID,
   EditButtonID,
   DeleteButtonID,
   UpButtonID,
   DownButtonID,
   RenameButtonID,
// ChainsListID             7005
// CommandsListID,       7002
// Re-Use IDs from BatchProcessDialog.
   ApplyToProjectButtonID = ApplyToProjectID,
   ApplyToFilesButtonID = ApplyToFilesID,
};

BEGIN_EVENT_TABLE(EditChainsDialog, BatchProcessDialog)
   EVT_LIST_ITEM_SELECTED(ChainsListID, EditChainsDialog::OnChainSelected)
   EVT_LIST_ITEM_SELECTED(CommandsListID, EditChainsDialog::OnListSelected)
   EVT_LIST_BEGIN_LABEL_EDIT(ChainsListID, EditChainsDialog::OnChainsBeginEdit)
   EVT_LIST_END_LABEL_EDIT(ChainsListID, EditChainsDialog::OnChainsEndEdit)
   EVT_BUTTON(AddButtonID, EditChainsDialog::OnAdd)
   EVT_BUTTON(RemoveButtonID, EditChainsDialog::OnRemove)
   EVT_BUTTON(RenameButtonID, EditChainsDialog::OnRename)
   EVT_SIZE(EditChainsDialog::OnSize)

   EVT_LIST_ITEM_ACTIVATED(CommandsListID, EditChainsDialog::OnCommandActivated)
   EVT_BUTTON(InsertButtonID, EditChainsDialog::OnInsert)
   EVT_BUTTON(EditButtonID, EditChainsDialog::OnEditCommandParams)
   EVT_BUTTON(DeleteButtonID, EditChainsDialog::OnDelete)
   EVT_BUTTON(UpButtonID, EditChainsDialog::OnUp)
   EVT_BUTTON(DownButtonID, EditChainsDialog::OnDown)
   EVT_BUTTON(DefaultsButtonID, EditChainsDialog::OnDefaults)

   EVT_BUTTON(wxID_OK, EditChainsDialog::OnOK)
   EVT_BUTTON(wxID_CANCEL, EditChainsDialog::OnCancel)

   EVT_KEY_DOWN(EditChainsDialog::OnKeyDown)
END_EVENT_TABLE()

enum {
   BlankColumn,
   ItemNumberColumn,
   ActionColumn,
   ParamsColumn,
};

/// Constructor
EditChainsDialog::EditChainsDialog(wxWindow * parent, bool bExpanded):
   BatchProcessDialog(parent, true)
{
   mbExpanded = bExpanded;
   SetLabel(_("Edit Chains"));         // Provide visual label
   SetName(_("Edit Chains"));          // Provide audible label
   SetTitle(_("Edit Chains"));

   mChanged = false;
   mSelectedCommand = 0;

   if( mbExpanded )
      Populate();
   else
      BatchProcessDialog::Populate();
}

EditChainsDialog::~EditChainsDialog()
{
}

/// Creates the dialog and its contents.
void EditChainsDialog::Populate()
{
   mCommandNames = BatchCommands::GetAllCommands();

   //------------------------- Main section --------------------
   ShuttleGui S(this, eIsCreating);
   PopulateOrExchange(S);
   // ----------------------- End of main section --------------

   // Get and validate the currently active chain
   mActiveChain = gPrefs->Read(wxT("/Batch/ActiveChain"), wxT(""));
   // Go populate the chains list.
   PopulateChains();

   // We have a bare list.  We need to add columns and content.
   PopulateList();

   // Layout and set minimum size of window
   Layout();
   Fit();
   SetSizeHints(GetSize());

   // Size and place window
   SetSize(wxSystemSettings::GetMetric(wxSYS_SCREEN_X) * 3 / 4,
           wxSystemSettings::GetMetric(wxSYS_SCREEN_Y) * 4 / 5);
   Center();

   // Set the column size for the chains list.
   wxSize sz = mChains->GetClientSize();
   mChains->SetColumnWidth(0, sz.x);

   // Size columns properly
   FitColumns();
}

/// Defines the dialog and does data exchange with it.
void EditChainsDialog::PopulateOrExchange(ShuttleGui & S)
{
   S.StartHorizontalLay(wxEXPAND, 1);
   {
      S.StartStatic(_("&Chains"));
      {
         // JKC: Experimenting with an alternative way to get multiline
         // translated strings to work correctly without very long lines.
         // My appologies Alexandre if this way didn't work either.
         //
         // With this method:
         //   1) it compiles fine under windows unicode and normal mode.
         //   2) xgettext source code has handling for the trailing '\'
         //
         // It remains to see if linux and mac can cope and if xgettext
         // actually does do fine with strings presented like this.
         // If it doesn't work out, revert to all-on-one-line.
         S.SetStyle(wxSUNKEN_BORDER | wxLC_REPORT | wxLC_HRULES | wxLC_SINGLE_SEL |
                    wxLC_EDIT_LABELS);
         mChains = S.Id(ChainsListID).AddListControlReportMode();
         // i18n-hint: This is the heading for a column in the edit chains dialog
         mChains->InsertColumn(0, _("Chain"), wxLIST_FORMAT_LEFT);
         S.StartHorizontalLay(wxCENTER, false);
         {
            S.Id(AddButtonID).AddButton(_("&Add"));
            mRemove = S.Id(RemoveButtonID).AddButton(_("&Remove"));
            mRename = S.Id(RenameButtonID).AddButton(_("Re&name"));
         }
         S.EndHorizontalLay();
      }
      S.EndStatic();

      S.StartVerticalLay( 1 );
      {
         S.StartStatic(_("C&hain (Double-Click or press SPACE to edit)"), true);
         {
            S.StartHorizontalLay(wxEXPAND,1);
            {
            
               S.SetStyle(wxSUNKEN_BORDER | wxLC_REPORT | wxLC_HRULES | wxLC_VRULES |
                          wxLC_SINGLE_SEL);
               mList = S.Id(CommandsListID).AddListControlReportMode();

               //An empty first column is a workaround - under Win98 the first column
               //can't be right aligned.
               mList->InsertColumn(BlankColumn, wxT(""), wxLIST_FORMAT_LEFT);
               /* i18n-hint: This is the number of the command in the list */
               mList->InsertColumn(ItemNumberColumn, _("Num"), wxLIST_FORMAT_RIGHT);
               mList->InsertColumn(ActionColumn, _("Command  "), wxLIST_FORMAT_RIGHT);
               mList->InsertColumn(ParamsColumn, _("Parameters"), wxLIST_FORMAT_LEFT);

               S.StartVerticalLay(0);
               {
                  S.Id(InsertButtonID).AddButton(_("&Insert"), wxALIGN_LEFT);
                  S.Id(EditButtonID).AddButton(_("&Edit"), wxALIGN_LEFT);
                  S.Id(DeleteButtonID).AddButton(_("De&lete"), wxALIGN_LEFT);
                  S.Id(UpButtonID).AddButton(_("Move &Up"), wxALIGN_LEFT);
                  S.Id(DownButtonID).AddButton(_("Move &Down"), wxALIGN_LEFT);
                  mDefaults = S.Id(DefaultsButtonID).AddButton(_("De&faults"));
               }
               S.EndVerticalLay();
            }
            S.EndHorizontalLay();
         }
         S.EndStatic();
         S.StartHorizontalLay(wxALIGN_RIGHT, false);
         {
            S.AddPrompt( _("Apply Chain to:") );
            S.Id(ApplyToProjectButtonID).AddButton(_("&Project"), wxALIGN_LEFT);
            S.Id(ApplyToFilesButtonID).AddButton(_("&Files..."), wxALIGN_LEFT);
            S.AddSpace( 40 );
            S.AddStandardButtons( eOkButton | eCancelButton | eHelpButton);
         }
         S.EndHorizontalLay();
      }
      S.EndVerticalLay();
   }
   S.EndHorizontalLay();

   return;
}

/// This clears and updates the contents of mList, the commands for the current chain.
void EditChainsDialog::PopulateList()
{
   mList->DeleteAllItems();

   for (int i = 0; i < mBatchCommands.GetCount(); i++) {
      AddItem(mBatchCommands.GetCommand(i),
              mBatchCommands.GetParams(i));
   }
   /*i18n-hint: This is the last item in a list.*/
   AddItem(_("- END -"), wxT(""));

   // Select the name in the list...this will fire an event.
   if (mSelectedCommand >= (int)mList->GetItemCount()) {
      mSelectedCommand = 0;
   }
   mList->SetItemState(mSelectedCommand, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
}

/// Add one item into mList
void EditChainsDialog::AddItem(const wxString &Action, const wxString &Params)
{
   // Translate internal command name to a friendly form
   auto item = make_iterator_range(mCommandNames).index_if(
      [&](const CommandName &name){ return Action == std::get<1>(name); }
   );
   auto friendlyName = item >= 0
      ? // wxGetTranslation
      std::get<0>( mCommandNames[item] )
      : Action;

   int i = mList->GetItemCount();

   mList->InsertItem(i, wxT(""));
   mList->SetItem(i, ItemNumberColumn, wxString::Format(wxT(" %02i"), i + 1));
   mList->SetItem(i, ActionColumn, friendlyName );
   mList->SetItem(i, ParamsColumn, Params );
}

void EditChainsDialog::UpdateMenus()
{
   // OK even on mac, as dialog is modal.
   GetActiveProject()->RebuildMenuBar();
}

void EditChainsDialog::UpdateDisplay( bool WXUNUSED(bExpanded) )
{
   //if(IsShown())
   //   DoUpdate();
}

bool EditChainsDialog::ChangeOK()
{
   if (mChanged) {
      wxString title;
      wxString msg;
      int id;

      title.Printf(_("%s changed"), mActiveChain);
      msg = _("Do you want to save the changes?");

      id = AudacityMessageBox(msg, title, wxYES_NO | wxCANCEL);
      if (id == wxCANCEL) {
         return false;
      }

      if (id == wxYES) {
         if (!mBatchCommands.WriteChain(mActiveChain)) {
            return false;
         }
      }

      mChanged = false;
   }

   return true;
}
/// An item in the chains list has been selected.
void EditChainsDialog::OnChainSelected(wxListEvent & event)
{
   if (!ChangeOK()) {
      event.Veto();
      return;
   }

   int item = event.GetIndex();

   mActiveChain = mChains->GetItemText(item);
   mBatchCommands.ReadChain(mActiveChain);
   if( !mbExpanded )
      return;
   
   if (mBatchCommands.IsFixed(mActiveChain)) {
      mRemove->Disable();
      mRename->Disable();
      mDefaults->Enable();
   }
   else {
      mRemove->Enable();
      mRename->Enable();
      mDefaults->Disable();
   }

   PopulateList();
}

/// An item in the chains list has been selected.
void EditChainsDialog::OnListSelected(wxListEvent & WXUNUSED(event))
{
   FitColumns();
}

/// The window has been resized.
void EditChainsDialog::OnSize(wxSizeEvent & WXUNUSED(event))
{
   // Refrsh the layout and re-fit the columns.
   Layout();
   if( !mbExpanded )
      return;
   FitColumns();
}

void EditChainsDialog::FitColumns()
{
   mList->SetColumnWidth(0, 0);  // First column width is zero, to hide it.

#if defined(__WXMAC__)
   // wxMac uses a hard coded width of 150 when wxLIST_AUTOSIZE_USEHEADER
   // is specified, so we calculate the width ourselves. This method may
   // work equally well on other platforms.
   for (size_t c = 1; c < mList->GetColumnCount(); c++) {
      wxListItem info;
      int width;

      mList->SetColumnWidth(c, wxLIST_AUTOSIZE);
      info.Clear();
      info.SetId(c);
      info.SetMask(wxLIST_MASK_TEXT | wxLIST_MASK_WIDTH);
      mList->GetColumn(c, info);

      mList->GetTextExtent(info.GetText(), &width, NULL);
      width += 2 * 4;    // 2 * kItemPadding - see listctrl_mac.cpp
      width += 16;       // kIconWidth - see listctrl_mac.cpp

      mList->SetColumnWidth(c, wxMax(width, mList->GetColumnWidth(c)));
   }

   // Looks strange, but it forces the horizontal scrollbar to get
   // drawn.  If not done, strange column sizing can occur if the
   // user attempts to resize the columns.
   mList->SetClientSize(mList->GetClientSize());
#else
   mList->SetColumnWidth(1, wxLIST_AUTOSIZE_USEHEADER);
   mList->SetColumnWidth(2, wxLIST_AUTOSIZE_USEHEADER);
   mList->SetColumnWidth(3, wxLIST_AUTOSIZE);
#endif

   int bestfit = mList->GetColumnWidth(3);
   int clientsize = mList->GetClientSize().GetWidth();
   int col1 = mList->GetColumnWidth(1);
   int col2 = mList->GetColumnWidth(2);
   bestfit = (bestfit > clientsize-col1-col2)? bestfit : clientsize-col1-col2;
   mList->SetColumnWidth(3, bestfit);

}

///
void EditChainsDialog::OnChainsBeginEdit(wxListEvent &event)
{
   int itemNo = event.GetIndex();

   wxString chain = mChains->GetItemText(itemNo);

   if (mBatchCommands.IsFixed(mActiveChain)) {
      wxBell();
      event.Veto();
   }
}

///
void EditChainsDialog::OnChainsEndEdit(wxListEvent &event)
{
   if (event.IsEditCancelled()) {
      return;
   }

   wxString newname = event.GetLabel();

   mBatchCommands.RenameChain(mActiveChain, newname);

   mActiveChain = newname;

   PopulateChains();
}

///
void EditChainsDialog::OnAdd(wxCommandEvent & WXUNUSED(event))
{
   while (true) {
      AudacityTextEntryDialog d(this,
                          _("Enter name of new chain"),
                          _("Name of new chain"));
      d.SetName(d.GetTitle());
      wxString name;

      if (d.ShowModal() == wxID_CANCEL) {
         return;
      }

      name = d.GetValue().Strip(wxString::both);

      if (name.Length() == 0) {
         AudacityMessageBox(_("Name must not be blank"),
                      GetTitle(),
                      wxOK | wxICON_ERROR,
                      this);
         continue;
      }

      if (name.Contains(wxFILE_SEP_PATH) ||
          name.Contains(wxFILE_SEP_PATH_UNIX)) {
         /*i18n-hint: The %c will be replaced with 'forbidden characters', like '/' and '\'.*/
         AudacityMessageBox(wxString::Format(_("Names may not contain '%c' and '%c'"),
                      wxFILE_SEP_PATH, wxFILE_SEP_PATH_UNIX),
                      GetTitle(),
                      wxOK | wxICON_ERROR,
                      this);
         continue;
      }

      mBatchCommands.AddChain(name);

      mActiveChain = name;

      PopulateChains();
      UpdateMenus();

      break;
   }
}

///
void EditChainsDialog::OnRemove(wxCommandEvent & WXUNUSED(event))
{
   long item = mChains->GetNextItem(-1,
                                    wxLIST_NEXT_ALL,
                                    wxLIST_STATE_SELECTED);
   if (item == -1) {
      return;
   }

   wxString name = mChains->GetItemText(item);
   AudacityMessageDialog m(this,
   /*i18n-hint: %s will be replaced by the name of a file.*/
                     wxString::Format(_("Are you sure you want to delete %s?"), name),
                     GetTitle(),
                     wxYES_NO | wxICON_QUESTION);
   if (m.ShowModal() == wxID_NO) {
      return;
   }

   mBatchCommands.DeleteChain(name);

   if (item >= (mChains->GetItemCount() - 1) && item >= 0) {
      item--;
   }

   mActiveChain = mChains->GetItemText(item);

   PopulateChains();
   UpdateMenus();
}

///
void EditChainsDialog::OnRename(wxCommandEvent & WXUNUSED(event))
{
   long item = mChains->GetNextItem(-1,
                                    wxLIST_NEXT_ALL,
                                    wxLIST_STATE_SELECTED);
   if (item == -1) {
      return;
   }

   mChains->EditLabel(item);
   UpdateMenus();
}

/// An item in the list has been selected.
/// Bring up a dialog to allow its parameters to be edited.
void EditChainsDialog::OnCommandActivated(wxListEvent & WXUNUSED(event))
{
   wxCommandEvent dummy;
   OnEditCommandParams( dummy );
}

///
void EditChainsDialog::OnInsert(wxCommandEvent & WXUNUSED(event))
{
   long item = mList->GetNextItem(-1,
                                  wxLIST_NEXT_ALL,
                                  wxLIST_STATE_SELECTED);
   if (item == -1) {
      item = mList->GetItemCount()-1;
   }
   InsertCommandAt( item );
}

void EditChainsDialog::InsertCommandAt(int item)
{
   if (item == -1) {
      return;
   }

   BatchCommandDialog d(this, wxID_ANY);

   if (!d.ShowModal()) {
      return;
   }

   if(d.mSelectedCommand != wxT(""))
   {
      mBatchCommands.AddToChain(d.mSelectedCommand,
                                d.mSelectedParameters,
                                item);
      mChanged = true;
      mSelectedCommand = item + 1;
      PopulateList();
   }
}

void EditChainsDialog::OnEditCommandParams(wxCommandEvent & WXUNUSED(event))
{
   int item = mList->GetNextItem( -1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED );

   // LAST command in list is END.
   // If nothing selected, add at END.
   // If END selected, add at END.
   // When adding at end we use InsertCommandAt, so that a new command
   // can be chosen.
   int lastItem = mList->GetItemCount()-1;
   if( (item<0) || (item+1) == mList->GetItemCount() )
   {
      InsertCommandAt( lastItem );
      return;
   }

   // Just edit the parameters, and not the command.
   wxString command = mBatchCommands.GetCommand(item);
   wxString params  = mBatchCommands.GetParams(item);

   params = BatchCommands::PromptForParamsFor(command, params, this).Trim();

   mBatchCommands.DeleteFromChain(item);
   mBatchCommands.AddToChain(command,
                             params,
                             item);
   mChanged = true;
   mSelectedCommand = item;
   PopulateList();
}

///
void EditChainsDialog::OnDelete(wxCommandEvent & WXUNUSED(event))
{
   long item = mList->GetNextItem(-1,
                                  wxLIST_NEXT_ALL,
                                  wxLIST_STATE_SELECTED);
   if (item == -1 || item + 1 == mList->GetItemCount()) {
      return;
   }

   mBatchCommands.DeleteFromChain(item);
   mChanged = true;

   if (item >= (mList->GetItemCount() - 2) && item >= 0) {
      item--;
   }
   mSelectedCommand = item;
   PopulateList();
}

///
void EditChainsDialog::OnUp(wxCommandEvent & WXUNUSED(event))
{
   long item = mList->GetNextItem(-1,
                                  wxLIST_NEXT_ALL,
                                  wxLIST_STATE_SELECTED);
   if (item == -1 || item == 0 || item + 1 == mList->GetItemCount()) {
      return;
   }

   mBatchCommands.AddToChain(mBatchCommands.GetCommand(item),
                             mBatchCommands.GetParams(item),
                             item - 1);
   mBatchCommands.DeleteFromChain(item + 1);
   mChanged = true;
   mSelectedCommand = item - 1;
   PopulateList();
}

///
void EditChainsDialog::OnDown(wxCommandEvent & WXUNUSED(event))
{
   long item = mList->GetNextItem(-1,
                                  wxLIST_NEXT_ALL,
                                  wxLIST_STATE_SELECTED);
   if (item == -1 || item + 2 >= mList->GetItemCount()) {
      return;
   }

   mBatchCommands.AddToChain(mBatchCommands.GetCommand(item),
                             mBatchCommands.GetParams(item),
                             item + 2);
   mBatchCommands.DeleteFromChain(item);
   mChanged = true;
   mSelectedCommand = item + 1;
   PopulateList();
}

void EditChainsDialog::OnApplyToProject(wxCommandEvent & event)
{
   if( !SaveChanges() )
      return;
   BatchProcessDialog::OnApplyToProject( event );
}

void EditChainsDialog::OnApplyToFiles(wxCommandEvent & event)
{
   if( !SaveChanges() )
      return;
   BatchProcessDialog::OnApplyToFiles( event );
}

/// Select the empty Command chain.
void EditChainsDialog::OnDefaults(wxCommandEvent & WXUNUSED(event))
{
   mBatchCommands.RestoreChain(mActiveChain);

   mChanged = true;

   PopulateList();
}

bool EditChainsDialog::SaveChanges(){
   gPrefs->Write(wxT("/Batch/ActiveChain"), mActiveChain);
   gPrefs->Flush();

   if (mChanged) {
      if (!mBatchCommands.WriteChain(mActiveChain)) {
         return false;
      }
   }
   mChanged = false;
   return true;
}

/// Send changed values back to Prefs, and update Audacity.
void EditChainsDialog::OnOK(wxCommandEvent & WXUNUSED(event))
{
   if( !SaveChanges() )
      return;
   Hide();
   //EndModal(true);
}

///
void EditChainsDialog::OnCancel(wxCommandEvent & event)
{
   if (!ChangeOK()) {
      return;
   }
   Hide();
}

///
void EditChainsDialog::OnKeyDown(wxKeyEvent &event)
{
   if (event.GetKeyCode() == WXK_DELETE) {
      wxLogDebug(wxT("wxKeyEvent"));
   }

   event.Skip();
}
