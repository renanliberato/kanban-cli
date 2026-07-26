Feature: Card Detail View
  As a user
  I want to view and edit full card details
  So that I can see descriptions, labels, and metadata

  Background:
    Given a board with a card "test" in "To Do"

  Scenario: Open and close detail view
    When I launch the application
    When I press Enter
    When I press ESC
    Then the screen should show the headers "To Do", "Doing", and "Done"

  Scenario: Edit title in detail view
    When I launch the application
    When I press Enter
    When I press "t"
    When I press Backspace 4 times
    When I type "modified"
    When I press Enter
    When I press ESC
    Then the "To Do" column should contain "modified"

  Scenario: Edit description in detail view
    When I launch the application
    When I press Enter
    When I press "D"
    When I type "some description"
    When I press Enter
    When I press ESC
    Then the "To Do" column should contain "test"
