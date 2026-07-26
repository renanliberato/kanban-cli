Feature: Adaptive Layout
  As a user
  I want the board to adapt to narrow terminal widths
  So that I can use it on half-screen or small terminals

  Scenario: Narrow terminal shows single-column view with tabs
    Given a board with the following cards
      | title     | column |
      | todo-1    | To Do  |
      | doing-1   | Doing  |
    When I launch the application at 24x50
    Then the screen should show the headers "To Do", "Doing", and "Done"
    And the "To Do" column should contain "todo-1"

  Scenario: Tab cycles columns in narrow mode
    Given a board with the following cards
      | title     | column |
      | todo-1    | To Do  |
      | doing-1   | Doing  |
    When I launch the application at 24x50
    And I press Tab
    Then the "Doing" column should contain "doing-1"

  Scenario: Add card works in narrow mode
    Given a board with no cards
    When I launch the application at 24x50
    When I add a card "narrow-add" to "To Do"
    Then the "To Do" column should contain "narrow-add"

  Scenario: Resize to wide shows three columns
    Given a board with the following cards
      | title     | column |
      | todo-1    | To Do  |
      | doing-1   | Doing  |
    When I launch the application at 24x100
    Then the screen should show the headers "To Do", "Doing", and "Done"
    And the "To Do" column should contain "todo-1"
    And the "Doing" column should contain "doing-1"
