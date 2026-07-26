Feature: Undo
  As a user
  I want to undo the last destructive operation
  So that I can recover from accidental deletions, moves, or edits

  Scenario: Undo a card deletion restores the card
    Given a board with a card "important" in "To Do"
    When I launch the application
    When I delete the card "important"
    And I confirm the deletion
    When I press "u"
    Then the "To Do" column should contain "important"

  Scenario: Undo a card move restores original position
    Given a board with a card "mover" in "To Do"
    When I launch the application
    When I move the card "mover" right
    Then the "Doing" column should contain "mover"
    When I press "u"
    Then the "To Do" column should contain "mover"
    And the "Doing" column should not contain "mover"

  Scenario: Undo a title edit restores original title
    Given a board with a card "original" in "To Do"
    When I launch the application
    When I edit the card "original" to "modified"
    Then the "To Do" column should contain "modified"
    When I press "u"
    Then the "To Do" column should contain "original"
    And the "To Do" column should not contain "modified"

  Scenario: Status bar shows undo hint after destructive operation
    Given a board with a card "task" in "To Do"
    When I launch the application
    When I move the card "task" right
    Then the status bar should show "Undo"
